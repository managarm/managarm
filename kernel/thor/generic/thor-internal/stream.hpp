#pragma once

#include <stddef.h>
#include <string.h>
#include <atomic>
#include <optional>

#include <async/oneshot-event.hpp>
#include <async/queue.hpp>
#include <frg/container_of.hpp>
#include <frg/array.hpp>
#include <frg/vector.hpp>
#include <thor-internal/coroutine.hpp>
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

	async::oneshot_primitive completion;

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
			_packet->completion.raise();
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

	Stream(bool withCredentials = false);
	~Stream();

	// Submits a chain of operations to the stream.
	static void transmit(const LaneHandle &lane, StreamList &chain) {
		Submitter submitter;
		submitter.enqueue(lane, chain);
		submitter.run();
	}

	void shutdownLane(int lane);

	Credentials &credentials() {
		assert(_withCredentials);
		assert(_creds.has_value());
		return _creds.value();
	}

private:
	static void _cancelItem(StreamNode *item, Error error);

	std::atomic<int> _peerCount[2];

	frg::optional<Credentials> _creds;

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

	bool _withCredentials;
};

frg::tuple<LaneHandle, LaneHandle> createStream(bool withCredentials = false);

//---------------------------------------------------------------------------------------
// In-kernel stream utilities.
// Those are only used internally; not by the hel API.
//---------------------------------------------------------------------------------------

inline coroutine<Error> dismiss(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagDismiss, &packet);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return node.error();
}

inline coroutine<frg::tuple<Error, LaneHandle>> offer(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagOffer, &packet);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return frg::make_tuple(node.error(), node.lane());
}

inline coroutine<frg::tuple<Error, LaneHandle>> accept(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagAccept, &packet);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return frg::make_tuple(node.error(), node.lane());
}

inline coroutine<frg::tuple<Error, frg::array<char, 16>>> extractCredentials(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagExtractCredentials, &packet);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return frg::make_tuple(node.error(), node.credentials());
}

inline coroutine<Error> sendBuffer(LaneHandle lane, frg::unique_memory<KernelAlloc> buffer) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagSendKernelBuffer, &packet);
	node._inBuffer = std::move(buffer);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return node.error();
}

inline coroutine<frg::tuple<Error, frg::unique_memory<KernelAlloc>>> recvBuffer(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagRecvKernelBuffer, &packet);
	node._maxLength = SIZE_MAX;

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return frg::make_tuple(node.error(), node.transmitBuffer());
}

inline coroutine<Error> pushDescriptor(LaneHandle lane, AnyDescriptor descriptor) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagPushDescriptor, &packet);
	node._inDescriptor = std::move(descriptor);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return node.error();
}

inline coroutine<frg::tuple<Error, AnyDescriptor>> pullDescriptor(LaneHandle lane) {
	StreamPacket packet;
	StreamNode node;
	packet.setup(1);
	node.setup(kTagPullDescriptor, &packet);

	StreamList list;
	list.push_back(&node);
	Stream::transmit(lane, list);

	co_await packet.completion.wait();
	co_return frg::make_tuple(node.error(), node.descriptor());
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
