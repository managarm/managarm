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

	void complete(Args... args) override {
		_functor(frigg::move(args)...);
	}

private:
	F _functor;
};

struct StreamControl {
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

	explicit StreamControl(int tag)
	: _tag(tag) { }

	int tag() const {
		return _tag;
	}

	frigg::IntrusiveSharedLinkedItem<StreamControl> processQueueItem;

private:
	int _tag;
};

struct OfferBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagOffer;
	}

	explicit OfferBase()
	: StreamControl(kTagOffer) { }

	virtual void complete(Error error) = 0;
};

template<typename F>
using Offer = Realization<OfferBase, void(Error), F>;

struct AcceptBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagAccept;
	}

	explicit AcceptBase(frigg::WeakPtr<Universe> universe)
	: StreamControl(kTagAccept), _universe(frigg::move(universe)) { }

	virtual void complete(Error error, frigg::WeakPtr<Universe> universe,
			LaneDescriptor lane) = 0;
	
	frigg::WeakPtr<Universe> _universe;
};

template<typename F>
using Accept = Realization<
	AcceptBase,
	void(Error, frigg::WeakPtr<Universe>, LaneDescriptor),
	F
>;

struct ImbueCredentialsBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagImbueCredentials;
	}

	explicit ImbueCredentialsBase(const char * credentials_)
	: StreamControl(kTagImbueCredentials) {
		memcpy(credentials.data(), credentials_, 16);
	}

	virtual void complete(Error error) = 0;

	frigg::Array<char, 16> credentials;
};

template<typename F>
using ImbueCredentials = Realization<ImbueCredentialsBase, void(Error), F>;

struct ExtractCredentialsBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagExtractCredentials;
	}

	explicit ExtractCredentialsBase()
	: StreamControl(kTagExtractCredentials) { }

	virtual void complete(Error error, frigg::Array<char, 16> credentials) = 0;
};

template<typename F>
using ExtractCredentials = Realization<ExtractCredentialsBase,
		void(Error, frigg::Array<char, 16>), F>;

struct SendFromBufferBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagSendFromBuffer;
	}

	explicit SendFromBufferBase(frigg::UniqueMemory<KernelAlloc> buffer)
	: StreamControl(kTagSendFromBuffer), buffer(frigg::move(buffer)) { }

	virtual void complete(Error error) = 0;
	
	frigg::UniqueMemory<KernelAlloc> buffer;
};

template<typename F>
using SendFromBuffer = Realization<SendFromBufferBase, void(Error), F>;

struct RecvInlineBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagRecvInline;
	}

	explicit RecvInlineBase()
	: StreamControl(kTagRecvInline) { }

	virtual void complete(Error error, frigg::UniqueMemory<KernelAlloc> buffer) = 0;
};

template<typename F>
using RecvInline = Realization<
	RecvInlineBase,
	void(Error, frigg::UniqueMemory<KernelAlloc>),
	F
>;

struct RecvToBufferBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagRecvToBuffer;
	}

	explicit RecvToBufferBase(AnyBufferAccessor accessor)
	: StreamControl(kTagRecvToBuffer), accessor(frigg::move(accessor)) { }

	virtual void complete(Error error, size_t length) = 0;
	
	AnyBufferAccessor accessor;
};

template<typename F>
using RecvToBuffer = Realization<RecvToBufferBase, void(Error, size_t), F>;

struct PushDescriptorBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagPushDescriptor;
	}

	explicit PushDescriptorBase(AnyDescriptor lane)
	: StreamControl(kTagPushDescriptor), _lane(frigg::move(lane)) { }

	virtual void complete(Error error) = 0;

	AnyDescriptor _lane;
};

template<typename F>
using PushDescriptor = Realization<PushDescriptorBase, void(Error), F>;

struct PullDescriptorBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagPullDescriptor;
	}

	explicit PullDescriptorBase(frigg::WeakPtr<Universe> universe)
	: StreamControl(kTagPullDescriptor), _universe(frigg::move(universe)) { }

	virtual void complete(Error error, frigg::WeakPtr<Universe> universe,
			AnyDescriptor lane) = 0;
	
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
	
	template<typename F>
	LaneHandle submitOffer(int lane, F functor) {
		return _submitControl(lane, frigg::makeShared<Offer<F>>(*kernelAlloc,
				frigg::move(functor)));
	}
	
	template<typename F>
	LaneHandle submitAccept(int lane, frigg::WeakPtr<Universe> universe, F functor) {
		return _submitControl(lane, frigg::makeShared<Accept<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(universe)));
	}
	
	template<typename F>
	LaneHandle submitImbueCredentials(int lane, const char *credentials, F functor) {
		return _submitControl(lane, frigg::makeShared<ImbueCredentials<F>>(*kernelAlloc,
				frigg::move(functor), credentials));
	}
	
	template<typename F>
	LaneHandle submitExtractCredentials(int lane, F functor) {
		return _submitControl(lane, frigg::makeShared<ExtractCredentials<F>>(*kernelAlloc,
				frigg::move(functor)));
	}

	template<typename F>
	void submitSendBuffer(int lane, frigg::UniqueMemory<KernelAlloc> buffer, F functor) {
		_submitControl(lane, frigg::makeShared<SendFromBuffer<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(buffer)));
	}
	
	template<typename F>
	void submitRecvInline(int lane, F functor) {
		_submitControl(lane, frigg::makeShared<RecvInline<F>>(*kernelAlloc,
				frigg::move(functor)));
	}
	
	template<typename F>
	void submitRecvBuffer(int lane, AnyBufferAccessor accessor, F functor) {
		_submitControl(lane, frigg::makeShared<RecvToBuffer<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(accessor)));
	}
	
	template<typename F>
	void submitPushDescriptor(int lane, AnyDescriptor descriptor, F functor) {
		_submitControl(lane, frigg::makeShared<PushDescriptor<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(descriptor)));
	}
	
	template<typename F>
	void submitPullDescriptor(int lane, frigg::WeakPtr<Universe> universe, F functor) {
		_submitControl(lane, frigg::makeShared<PullDescriptor<F>>(*kernelAlloc,
				frigg::move(functor), frigg::move(universe)));
	}

private:
	static void _cancelItem(StreamControl *item);

	// submits an operation to the stream.
	LaneHandle _submitControl(int lane, frigg::SharedPtr<StreamControl> control);

	std::atomic<int> _peerCount[2];
	
	frigg::TicketLock _mutex;

	// protected by _mutex.
	frigg::IntrusiveSharedLinkedList<
		StreamControl,
		&StreamControl::processQueueItem
	> _processQueue[2];
	
	// protected by _mutex.
	frigg::LinkedList<frigg::SharedPtr<Stream>, KernelAlloc> _conversationQueue;
	
	// protected by _mutex.
	bool _laneBroken[2];
};

frigg::Tuple<LaneHandle, LaneHandle> createStream();

} // namespace thor

#endif // THOR_GENERIC_STREAM_HPP
