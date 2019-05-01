#ifndef THOR_GENERIC_STREAM_HPP
#define THOR_GENERIC_STREAM_HPP

#include <stddef.h>
#include <string.h>
#include <atomic>

#include <frg/container_of.hpp>
#include <frigg/array.hpp>
#include <frigg/linked.hpp>
#include <frigg/vector.hpp>
#include "core.hpp"
#include "error.hpp"
#include "kernel_heap.hpp"

namespace thor {

struct StreamPacket {
	friend struct Stream;
	friend struct StreamNode;

	StreamPacket()
	: _incompleteCount{0} { }

	void setup(unsigned int count, Worklet *transmitted) {
		_incompleteCount.store(count, std::memory_order_relaxed);
		_transmitted = transmitted;
	}

private:
	Worklet *_transmitted;

	std::atomic<unsigned int> _incompleteCount;
};

enum {
	kTagNull,
	kTagOffer,
	kTagAccept,
	kTagImbueCredentials,
	kTagExtractCredentials,
	kTagSendFromBuffer,
	kTagRecvInline,
	kTagRecvToBuffer,
	kTagPushDescriptor,
	kTagPullDescriptor
};

inline int getStreamOrientation(int tag) {
	switch(tag) {
	case kTagAccept:
	case kTagExtractCredentials:
	case kTagRecvInline:
	case kTagRecvToBuffer:
	case kTagPullDescriptor:
		return -1;
	case kTagOffer:
	case kTagImbueCredentials:
	case kTagSendFromBuffer:
	case kTagPushDescriptor:
		return 1;
	}
	return 0;
}

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
			WorkQueue::post(_packet->_transmitted);
	}

	LaneHandle _transmitLane;

private:
	int _tag;
	StreamPacket *_packet;

public:
	// ------------------------------------------------------------------------
	// Transmission inputs.
	// ------------------------------------------------------------------------

	frigg::Array<char, 16> _inCredentials;
	frigg::UniqueMemory<KernelAlloc> _inBuffer;
	AnyBufferAccessor _inAccessor;
	AnyDescriptor _inDescriptor;

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

	frigg::Array<char, 16> credentials() {
		return _transmitCredentials;
	}

	size_t actualLength() {
		return _actualLength;
	}

	frigg::UniqueMemory<KernelAlloc> transmitBuffer() {
		return std::move(_transmitBuffer);
	}

	const frigg::Array<char, 16> &transmitCredentials() {
		return _transmitCredentials;
	}

	LaneHandle lane() {
		return std::move(_lane);
	}

	AnyDescriptor descriptor() {
		return std::move(_descriptor);
	}

public:
	Error _error;
	frigg::Array<char, 16> _transmitCredentials;
	size_t _actualLength;
	frigg::UniqueMemory<KernelAlloc> _transmitBuffer;
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

struct Stream {
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

	frigg::TicketLock _mutex;

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

frigg::Tuple<LaneHandle, LaneHandle> createStream();

//---------------------------------------------------------------------------------------
// In-kernel stream utilities.
// Those are only used by servers; not by the hel API.
//---------------------------------------------------------------------------------------

template<typename Base, typename Signature, typename F>
struct StreamNodeAdapter;

template<typename Base, typename... Args, typename F>
struct StreamNodeAdapter<Base, void(Args...), F> : Base {
	template<typename... CArgs>
	explicit StreamNodeAdapter(F functor, CArgs &&... args)
	: Base(frigg::forward<CArgs>(args)...), _functor(frigg::move(functor)) { }

	void callback(Args... args) override {
		_functor(frigg::move(args)...);
		frigg::destruct(*kernelAlloc, this);
	}

private:
	F _functor;
};

struct OfferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagOffer;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &OfferBase::worklet);
		packet->callback(packet->error(), packet->lane());
	}

	explicit OfferBase() {
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagOffer, this);
	}

	virtual void callback(Error error, LaneHandle handle) = 0;

	Worklet worklet;
};

template<typename F>
using Offer = StreamNodeAdapter<OfferBase, void(Error, LaneHandle), F>;

struct AcceptBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagAccept;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &AcceptBase::worklet);
		packet->callback(packet->error(), packet->lane());
	}

	explicit AcceptBase() {
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagAccept, this);
	}

	virtual void callback(Error error, LaneHandle lane) = 0;

	Worklet worklet;
};

template<typename F>
using Accept = StreamNodeAdapter<
	AcceptBase,
	void(Error, LaneHandle),
	F
>;

struct ImbueCredentialsBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagImbueCredentials;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &ImbueCredentialsBase::worklet);
		packet->callback(packet->error());
	}

	explicit ImbueCredentialsBase(const char * credentials_) {
		memcpy(_inCredentials.data(), credentials_, 16);
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagImbueCredentials, this);
	}

	virtual void callback(Error error) = 0;

	Worklet worklet;
};

template<typename F>
using ImbueCredentials = StreamNodeAdapter<ImbueCredentialsBase, void(Error), F>;

struct ExtractCredentialsBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagExtractCredentials;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &ExtractCredentialsBase::worklet);
		packet->callback(packet->error(), packet->transmitCredentials());
	}

	explicit ExtractCredentialsBase() {
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagExtractCredentials, this);
	}

	virtual void callback(Error error, frigg::Array<char, 16> credentials) = 0;

	Worklet worklet;
};

template<typename F>
using ExtractCredentials = StreamNodeAdapter<ExtractCredentialsBase,
		void(Error, frigg::Array<char, 16>), F>;

struct SendFromBufferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagSendFromBuffer;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &SendFromBufferBase::worklet);
		packet->callback(packet->error());
	}

	explicit SendFromBufferBase(frigg::UniqueMemory<KernelAlloc> buffer) {
		_inBuffer = frigg::move(buffer);
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagSendFromBuffer, this);
	}

	virtual void callback(Error error) = 0;

	Worklet worklet;
};

template<typename F>
using SendFromBuffer = StreamNodeAdapter<SendFromBufferBase, void(Error), F>;

struct RecvInlineBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagRecvInline;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &RecvInlineBase::worklet);
		packet->callback(packet->error(), packet->transmitBuffer());
	}

	explicit RecvInlineBase() {
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagRecvInline, this);
	}

	virtual void callback(Error error, frigg::UniqueMemory<KernelAlloc> buffer) = 0;

	Worklet worklet;
};

template<typename F>
using RecvInline = StreamNodeAdapter<
	RecvInlineBase,
	void(Error, frigg::UniqueMemory<KernelAlloc>),
	F
>;

struct RecvToBufferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagRecvToBuffer;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &RecvToBufferBase::worklet);
		packet->callback(packet->error(), packet->actualLength());
	}

	explicit RecvToBufferBase(AnyBufferAccessor accessor) {
		_inAccessor = frigg::move(accessor);
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagRecvToBuffer, this);
	}

	virtual void callback(Error error, size_t length) = 0;

	Worklet worklet;
};

template<typename F>
using RecvToBuffer = StreamNodeAdapter<RecvToBufferBase, void(Error, size_t), F>;

struct PushDescriptorBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagPushDescriptor;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &PushDescriptorBase::worklet);
		packet->callback(packet->error());
	}

	explicit PushDescriptorBase(AnyDescriptor lane) {
		_inDescriptor = frigg::move(lane);
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagPushDescriptor, this);
	}

	virtual void callback(Error error) = 0;

	Worklet worklet;
};

template<typename F>
using PushDescriptor = StreamNodeAdapter<PushDescriptorBase, void(Error), F>;

struct PullDescriptorBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagPullDescriptor;
	}

	static void transmitted(Worklet *base) {
		auto packet = frg::container_of(base, &PullDescriptorBase::worklet);
		packet->callback(packet->error(), packet->descriptor());
	}

	explicit PullDescriptorBase() {
		worklet.setup(&transmitted);
		StreamPacket::setup(1, &worklet);
		StreamNode::setup(kTagPullDescriptor, this);
	}

	virtual void callback(Error error, AnyDescriptor descriptor) = 0;

	Worklet worklet;
};

template<typename F>
using PullDescriptor = StreamNodeAdapter<
	PullDescriptorBase,
	void(Error, AnyDescriptor),
	F
>;

template<typename F>
void submitOffer(LaneHandle lane, F functor) {
	StreamList list;
	list.push_back(frigg::construct<Offer<F>>(*kernelAlloc,
			frigg::move(functor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitAccept(LaneHandle lane, F functor) {
	StreamList list;
	list.push_back(frigg::construct<Accept<F>>(*kernelAlloc,
			frigg::move(functor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitImbueCredentials(LaneHandle lane, const char *credentials, F functor) {
	StreamList list;
	list.push_back(frigg::construct<ImbueCredentials<F>>(*kernelAlloc,
			frigg::move(functor), credentials));
	Stream::transmit(lane, list);
}

template<typename F>
void submitExtractCredentials(LaneHandle lane, F functor) {
	StreamList list;
	list.push_back(frigg::construct<ExtractCredentials<F>>(*kernelAlloc,
			frigg::move(functor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitSendBuffer(LaneHandle lane, frigg::UniqueMemory<KernelAlloc> buffer, F functor) {
	StreamList list;
	list.push_back(frigg::construct<SendFromBuffer<F>>(*kernelAlloc,
			frigg::move(functor), frigg::move(buffer)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitRecvInline(LaneHandle lane, F functor) {
	StreamList list;
	list.push_back(frigg::construct<RecvInline<F>>(*kernelAlloc,
			frigg::move(functor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitRecvBuffer(LaneHandle lane, AnyBufferAccessor accessor, F functor) {
	StreamList list;
	list.push_back(frigg::construct<RecvToBuffer<F>>(*kernelAlloc,
			frigg::move(functor), frigg::move(accessor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitPushDescriptor(LaneHandle lane, AnyDescriptor descriptor, F functor) {
	StreamList list;
	list.push_back(frigg::construct<PushDescriptor<F>>(*kernelAlloc,
			frigg::move(functor), frigg::move(descriptor)));
	Stream::transmit(lane, list);
}

template<typename F>
void submitPullDescriptor(LaneHandle lane, F functor) {
	StreamList list;
	list.push_back(frigg::construct<PullDescriptor<F>>(*kernelAlloc,
			frigg::move(functor)));
	Stream::transmit(lane, list);
}

} // namespace thor

#endif // THOR_GENERIC_STREAM_HPP
