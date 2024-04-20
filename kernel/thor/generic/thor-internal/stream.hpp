#pragma once

#include <stddef.h>
#include <string.h>
#include <atomic>

#include <async/oneshot-event.hpp>
#include <async/queue.hpp>
#include <frg/container_of.hpp>
#include <frg/array.hpp>
#include <frg/vector.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

struct StreamPacket {
	friend struct Stream;
	friend struct StreamNode;

	StreamPacket()
	: _incompleteCount{0} { }

	void setup(unsigned int count) {
		_incompleteCount.store(count, std::memory_order_relaxed);
	}

protected:
	virtual void completePacket() = 0;
	~StreamPacket() = default;

private:
	std::atomic<unsigned int> _incompleteCount;
};

enum {
	kTagNull,
	kTagDismiss,
	kTagOffer,
	kTagAccept,
	kTagImbueCredentials,
	kTagExtractCredentials,
	kTagSendKernelBuffer,
	kTagSendFlow,
	kTagRecvKernelBuffer,
	kTagRecvFlow,
	kTagPushDescriptor,
	kTagPullDescriptor
};

inline int getStreamOrientation(int tag) {
	switch(tag) {
	case kTagAccept:
	case kTagExtractCredentials:
	case kTagRecvKernelBuffer:
	case kTagRecvFlow:
	case kTagPullDescriptor:
		return -1;
	case kTagOffer:
	case kTagImbueCredentials:
	case kTagSendKernelBuffer:
	case kTagSendFlow:
	case kTagPushDescriptor:
		return 1;
	}
	return 0;
}

inline bool usesFlowProtocol(int tag) {
	return tag == kTagSendFlow || tag == kTagRecvFlow;
}

struct FlowPacket {
	void *data = nullptr;
	size_t size = 0;
	bool terminate = false;
	bool fault = false;
};

struct StreamNode {
	friend struct Stream;

	StreamNode() = default;

	StreamNode(const StreamNode &) = delete;

	StreamNode &operator= (const StreamNode &) = delete;

	int tag() const {
		return _tag;
	}

	void setup(int tag, StreamPacket *packet) {
		_tag = tag;
		_packet = packet;
	}

	frg::default_list_hook<StreamNode> processQueueItem;

	void complete() {
		auto n = _packet->_incompleteCount.fetch_sub(1, std::memory_order_acq_rel);
		assert(n > 0);
		if(n == 1)
			_packet->completePacket();
	}

	LaneHandle _transmitLane;

	int _tag;
	StreamPacket *_packet;

public:
	// ------------------------------------------------------------------------
	// Transmission inputs.
	// ------------------------------------------------------------------------

	frg::array<char, 16> _inCredentials;
	size_t _maxLength;
	frg::unique_memory<KernelAlloc> _inBuffer;
	AnyDescriptor _inDescriptor;

	StreamNode *peerNode = nullptr;

	async::oneshot_event issueFlow;
	async::queue<FlowPacket, KernelAlloc> flowQueue{*kernelAlloc};

	// List of StreamNodes that will be submitted to the ancillary lane on offer/accept.
	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> ancillaryChain;

	// ------------------------------------------------------------------------
	// Transmission outputs.
	// ------------------------------------------------------------------------

	// TODO: Initialize outputs to zero to avoid leaks to usermode.
public:
	Error error() {
		return _error;
	}

	frg::array<char, 16> credentials() {
		return _transmitCredentials;
	}

	size_t actualLength() {
		return _actualLength;
	}

	frg::unique_memory<KernelAlloc> transmitBuffer() {
		return std::move(_transmitBuffer);
	}

	const frg::array<char, 16> &transmitCredentials() {
		return _transmitCredentials;
	}

	LaneHandle lane() {
		return std::move(_lane);
	}

	AnyDescriptor descriptor() {
		return std::move(_descriptor);
	}

public:
	Error _error{};
	frg::array<char, 16> _transmitCredentials;
	size_t _actualLength = 0;
	frg::unique_memory<KernelAlloc> _transmitBuffer;
	LaneHandle _lane;
	AnyDescriptor _descriptor;
};

using StreamList = frg::intrusive_list<
	StreamNode,
	frg::locate_member<
		StreamNode,
		frg::default_list_hook<StreamNode>,
		&StreamNode::processQueueItem
	>
>;

struct Stream final {
	struct Submitter {
		void enqueue(const LaneHandle &lane, StreamList &chain);

		void run();

	private:
		StreamList _pending;
	};

	// manage the peer counter of each lane.
	// incrementing a peer counter that is already at zero is undefined.
	// decrementPeers() returns true if the counter reaches zero.
	static void incrementPeers(Stream *stream, int lane);
	static bool decrementPeers(Stream *stream, int lane);

	Stream();
	~Stream();

	// Submits a chain of operations to the stream.
	static void transmit(const LaneHandle &lane, StreamList &chain) {
		Submitter submitter;
		submitter.enqueue(lane, chain);
		submitter.run();
	}

	void shutdownLane(int lane);

private:
	static void _cancelItem(StreamNode *item, Error error);

	std::atomic<int> _peerCount[2];

	frg::ticket_spinlock _mutex;

	// protected by _mutex.
	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> _processQueue[2];

	// Protected by _mutex.
	// Further submissions cannot happen (lane went out-of-scope).
	// Submissions to the paired lane return end-of-lane errors.
	bool _laneBroken[2];
	// Submissions are disallowed and return lane-shutdown errors.
	// Submissions to the paired lane return end-of-lane errors.
	bool _laneShutDown[2];
};

frg::tuple<LaneHandle, LaneHandle> createStream();

//---------------------------------------------------------------------------------------
// In-kernel stream utilities.
// Those are only used internally; not by the hel API.
//---------------------------------------------------------------------------------------

struct DismissSender {
	LaneHandle lane;
};

template<typename R>
struct DismissOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagDismiss, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	DismissOperation(DismissSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	DismissOperation(const DismissOperation &) = delete;

	DismissOperation &operator= (const DismissOperation &) = delete;

private:
	void completePacket() override {
		async::execution::set_value(receiver_, error());
	}

	DismissSender s_;
	R receiver_;
};

template<typename R>
inline DismissOperation<R> connect(DismissSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<DismissSender, Error>
operator co_await(DismissSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct OfferSender {
	LaneHandle lane;
};

template<typename R>
struct OfferOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagOffer, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	OfferOperation(OfferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	OfferOperation(const OfferOperation &) = delete;

	OfferOperation &operator= (const OfferOperation &) = delete;

private:
	void completePacket() override {
		async::execution::set_value(receiver_,
				frg::make_tuple(error(), lane()));
	}

	OfferSender s_;
	R receiver_;
};

template<typename R>
inline OfferOperation<R> connect(OfferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<OfferSender, frg::tuple<Error, LaneHandle>>
operator co_await(OfferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct AcceptSender {
	LaneHandle lane;
};

template<typename R>
struct AcceptOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagAccept, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	AcceptOperation(AcceptSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	AcceptOperation(const AcceptOperation &) = delete;

	AcceptOperation &operator= (const AcceptOperation &) = delete;

private:
	void completePacket() override {
		async::execution::set_value(receiver_,
				frg::make_tuple(error(), lane()));
	}

	AcceptSender s_;
	R receiver_;
};

template<typename R>
inline AcceptOperation<R> connect(AcceptSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<AcceptSender, frg::tuple<Error, LaneHandle>>
operator co_await(AcceptSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct ExtractCredentialsSender {
	LaneHandle lane;
};

template<typename R>
struct ExtractCredentialsOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagExtractCredentials, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	ExtractCredentialsOperation(ExtractCredentialsSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	void completePacket() override {
		async::execution::set_value(receiver_,
				frg::make_tuple(error(), credentials()));
	}

	ExtractCredentialsSender s_;
	R receiver_;
};

template<typename R>
inline ExtractCredentialsOperation<R> connect(ExtractCredentialsSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<ExtractCredentialsSender, frg::tuple<Error, frg::array<char, 16>>>
operator co_await(ExtractCredentialsSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct SendBufferSender {
	LaneHandle lane;
	frg::unique_memory<KernelAlloc> buffer;
};

template<typename R>
struct SendBufferOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagSendKernelBuffer, this);
		_inBuffer = std::move(s_.buffer);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	SendBufferOperation(SendBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	void completePacket() override {
		async::execution::set_value(receiver_, error());
	}

	SendBufferSender s_;
	R receiver_;
};

template<typename R>
inline SendBufferOperation<R> connect(SendBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<SendBufferSender, Error>
operator co_await(SendBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct RecvBufferSender {
	LaneHandle lane;
};

template<typename R>
struct RecvBufferOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagRecvKernelBuffer, this);
		_maxLength = SIZE_MAX;

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	RecvBufferOperation(RecvBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	void completePacket() override {
		async::execution::set_value(receiver_,
				frg::make_tuple(error(), transmitBuffer()));
	}

	RecvBufferSender s_;
	R receiver_;
};

template<typename R>
inline RecvBufferOperation<R> connect(RecvBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<RecvBufferSender, frg::tuple<Error, frg::unique_memory<KernelAlloc>>>
operator co_await(RecvBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PushDescriptorSender {
	LaneHandle lane;
	AnyDescriptor descriptor;
};

template<typename R>
struct PushDescriptorOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagPushDescriptor, this);
		_inDescriptor = std::move(s_.descriptor);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	PushDescriptorOperation(PushDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	void completePacket() override {
		async::execution::set_value(receiver_, error());
	}

	PushDescriptorSender s_;
	R receiver_;
};

template<typename R>
inline PushDescriptorOperation<R> connect(PushDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PushDescriptorSender, Error>
operator co_await(PushDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PullDescriptorSender {
	LaneHandle lane;
};

template<typename R>
struct PullDescriptorOperation final : private StreamPacket, private StreamNode {
	void start() {
		StreamPacket::setup(1);
		StreamNode::setup(kTagPullDescriptor, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	PullDescriptorOperation(PullDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	void completePacket() override {
		async::execution::set_value(receiver_,
				frg::make_tuple(error(), descriptor()));
	}

	PullDescriptorSender s_;
	R receiver_;
};

template<typename R>
inline PullDescriptorOperation<R> connect(PullDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PullDescriptorSender, frg::tuple<Error, AnyDescriptor>>
operator co_await(PullDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

// Returns true if an IPC error is caused by the remote side not following the protocol.
inline bool isRemoteIpcError(Error e) {
	switch(e) {
		case Error::bufferTooSmall:
		case Error::transmissionMismatch:
			return true;
		default:
			return false;
	}
}


} // namespace thor
