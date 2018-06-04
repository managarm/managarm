#ifndef THOR_GENERIC_STREAM_HPP
#define THOR_GENERIC_STREAM_HPP

#include <atomic>
#include <frigg/array.hpp>
#include <frigg/linked.hpp>
#include <frigg/vector.hpp>
#include "core.hpp"
#include "error.hpp"
#include "kernel_heap.hpp"

namespace thor {

template<typename Base, typename Signature, typename F>
struct Realization;

template<typename Base, typename... Args, typename F>
struct Realization<Base, void(Args...), F> : Base {
	template<typename... CArgs>
	explicit Realization(F functor, CArgs &&... args)
	: Base(frigg::forward<CArgs>(args)...), _functor(frigg::move(functor)) { }

	void callback(Args... args) override {
		_functor(frigg::move(args)...);
		frigg::destruct(*kernelAlloc, this);
	}

private:
	F _functor;
};

struct StreamPacket {
	friend struct Stream;
	friend struct StreamNode;
	
	StreamPacket(void (*transmitted)(StreamPacket *))
	: _transmitted{transmitted}, _incompleteCount{0} { }

private:
	void (*_transmitted)(StreamPacket *);

	std::atomic<unsigned int> _incompleteCount;
};

struct StreamNode {
	friend struct Stream;

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

	explicit StreamNode(int tag, StreamPacket *packet)
	: _tag(tag), _packet{packet} {
		auto n = _packet->_incompleteCount.load(std::memory_order_relaxed);
		_packet->_incompleteCount.store(n + 1, std::memory_order_relaxed);
	}

	StreamNode(const StreamNode &) = delete;

	StreamNode &operator= (const StreamNode &) = delete;

	int tag() const {
		return _tag;
	}

	frg::default_list_hook<StreamNode> processQueueItem;

	void complete() {
		auto n = _packet->_incompleteCount.fetch_sub(1, std::memory_order_acq_rel);
		assert(n > 0);
		if(n == 1)
			_packet->_transmitted(_packet);
	}

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

	// ------------------------------------------------------------------------
	// Transmission outputs.
	// ------------------------------------------------------------------------

	// TODO: Initialize outputs to zero to avoid leaks to usermode.
public:
	Error error() {
		return _error;
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

struct OfferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagOffer;
	}

	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<OfferBase *>(base);
		packet->callback(packet->error());
	}

	explicit OfferBase()
	: StreamPacket(&transmitted), StreamNode(kTagOffer, this) { }

	virtual void callback(Error error) = 0;
};

template<typename F>
using Offer = Realization<OfferBase, void(Error), F>;

struct AcceptBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagAccept;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<AcceptBase *>(base);
		packet->callback(packet->error(), packet->_universe, LaneDescriptor{packet->lane()});
	}

	explicit AcceptBase(frigg::WeakPtr<Universe> universe)
	: StreamPacket(&transmitted), StreamNode(kTagAccept, this),
			_universe(frigg::move(universe)) { }

	virtual void callback(Error error, frigg::WeakPtr<Universe> universe,
			LaneDescriptor lane) = 0;
	
	frigg::WeakPtr<Universe> _universe;
};

template<typename F>
using Accept = Realization<
	AcceptBase,
	void(Error, frigg::WeakPtr<Universe>, LaneDescriptor),
	F
>;

struct ImbueCredentialsBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagImbueCredentials;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<ImbueCredentialsBase *>(base);
		packet->callback(packet->error());
	}

	explicit ImbueCredentialsBase(const char * credentials_)
	: StreamPacket(&transmitted), StreamNode(kTagImbueCredentials, this) {
		memcpy(_inCredentials.data(), credentials_, 16);
	}

	virtual void callback(Error error) = 0;
};

template<typename F>
using ImbueCredentials = Realization<ImbueCredentialsBase, void(Error), F>;

struct ExtractCredentialsBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagExtractCredentials;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<ExtractCredentialsBase *>(base);
		packet->callback(packet->error(), packet->transmitCredentials());
	}

	explicit ExtractCredentialsBase()
	: StreamPacket(&transmitted), StreamNode(kTagExtractCredentials, this) { }

	virtual void callback(Error error, frigg::Array<char, 16> credentials) = 0;
};

template<typename F>
using ExtractCredentials = Realization<ExtractCredentialsBase,
		void(Error, frigg::Array<char, 16>), F>;

struct SendFromBufferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagSendFromBuffer;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<SendFromBufferBase *>(base);
		packet->callback(packet->error());
	}

	explicit SendFromBufferBase(frigg::UniqueMemory<KernelAlloc> buffer)
	: StreamPacket(&transmitted), StreamNode(kTagSendFromBuffer, this) {
		_inBuffer = frigg::move(buffer);
	}

	virtual void callback(Error error) = 0;
};

template<typename F>
using SendFromBuffer = Realization<SendFromBufferBase, void(Error), F>;

struct RecvInlineBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagRecvInline;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<RecvInlineBase *>(base);
		packet->callback(packet->error(), packet->transmitBuffer());
	}

	explicit RecvInlineBase()
	: StreamPacket(&transmitted), StreamNode(kTagRecvInline, this) { }

	virtual void callback(Error error, frigg::UniqueMemory<KernelAlloc> buffer) = 0;
};

template<typename F>
using RecvInline = Realization<
	RecvInlineBase,
	void(Error, frigg::UniqueMemory<KernelAlloc>),
	F
>;

struct RecvToBufferBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagRecvToBuffer;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<RecvToBufferBase *>(base);
		packet->callback(packet->error(), packet->actualLength());
	}

	explicit RecvToBufferBase(AnyBufferAccessor accessor)
	: StreamPacket(&transmitted), StreamNode(kTagRecvToBuffer, this) {
		_inAccessor = frigg::move(accessor);
	}

	virtual void callback(Error error, size_t length) = 0;
};

template<typename F>
using RecvToBuffer = Realization<RecvToBufferBase, void(Error, size_t), F>;

struct PushDescriptorBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagPushDescriptor;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<PushDescriptorBase *>(base);
		packet->callback(packet->error());
	}

	explicit PushDescriptorBase(AnyDescriptor lane)
	: StreamPacket(&transmitted), StreamNode(kTagPushDescriptor, this) {
		_inDescriptor = frigg::move(lane);
	}

	virtual void callback(Error error) = 0;
};

template<typename F>
using PushDescriptor = Realization<PushDescriptorBase, void(Error), F>;

struct PullDescriptorBase : StreamPacket, StreamNode {
	static bool classOf(const StreamNode &base) {
		return base.tag() == kTagPullDescriptor;
	}
	
	static void transmitted(StreamPacket *base) {
		auto packet = static_cast<PullDescriptorBase *>(base);
		packet->callback(packet->error(), packet->_universe, packet->descriptor());
	}

	explicit PullDescriptorBase(frigg::WeakPtr<Universe> universe)
	: StreamPacket(&transmitted), StreamNode(kTagPullDescriptor, this),
			_universe(frigg::move(universe)) { }

	virtual void callback(Error error, frigg::WeakPtr<Universe> universe,
			AnyDescriptor descriptor) = 0;
	
	frigg::WeakPtr<Universe> _universe;
};

template<typename F>
using PullDescriptor = Realization<
	PullDescriptorBase,
	void(Error, frigg::WeakPtr<Universe>, AnyDescriptor),
	F
>;

struct Stream {
	// manage the peer counter of each lane.
	// incrementing a peer counter that is already at zero is undefined.
	// decrementPeers() returns true if the counter reaches zero.
	static void incrementPeers(Stream *stream, int lane);
	static bool decrementPeers(Stream *stream, int lane);

	Stream();
	~Stream();

	void shutdownLane(int lane);

	template<typename F>
	LaneHandle submitOffer(int lane, F functor) {
		return _submitControl(lane, frigg::construct<Offer<F>>(*kernelAlloc,
				frigg::move(functor)));
	}
	
	template<typename F>
	LaneHandle submitAccept(int lane, frigg::WeakPtr<Universe> universe, F functor) {
		return _submitControl(lane, frigg::construct<Accept<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(universe)));
	}
	
	template<typename F>
	LaneHandle submitImbueCredentials(int lane, const char *credentials, F functor) {
		return _submitControl(lane, frigg::construct<ImbueCredentials<F>>(*kernelAlloc,
				frigg::move(functor), credentials));
	}
	
	template<typename F>
	LaneHandle submitExtractCredentials(int lane, F functor) {
		return _submitControl(lane, frigg::construct<ExtractCredentials<F>>(*kernelAlloc,
				frigg::move(functor)));
	}

	template<typename F>
	void submitSendBuffer(int lane, frigg::UniqueMemory<KernelAlloc> buffer, F functor) {
		_submitControl(lane, frigg::construct<SendFromBuffer<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(buffer)));
	}
	
	template<typename F>
	void submitRecvInline(int lane, F functor) {
		_submitControl(lane, frigg::construct<RecvInline<F>>(*kernelAlloc,
				frigg::move(functor)));
	}
	
	template<typename F>
	void submitRecvBuffer(int lane, AnyBufferAccessor accessor, F functor) {
		_submitControl(lane, frigg::construct<RecvToBuffer<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(accessor)));
	}
	
	template<typename F>
	void submitPushDescriptor(int lane, AnyDescriptor descriptor, F functor) {
		_submitControl(lane, frigg::construct<PushDescriptor<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(descriptor)));
	}
	
	template<typename F>
	void submitPullDescriptor(int lane, frigg::WeakPtr<Universe> universe, F functor) {
		_submitControl(lane, frigg::construct<PullDescriptor<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(universe)));
	}

private:
	static void _cancelItem(StreamNode *item, Error error);

	// submits an operation to the stream.
	LaneHandle _submitControl(int lane, StreamNode *control);

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

	// protected by _mutex.
	frigg::LinkedList<frigg::SharedPtr<Stream>, KernelAlloc> _conversationQueue;
	
	// protected by _mutex.
	bool _laneBroken[2];
};

frigg::Tuple<LaneHandle, LaneHandle> createStream();

} // namespace thor

#endif // THOR_GENERIC_STREAM_HPP
