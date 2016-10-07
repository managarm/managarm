
#include <atomic>

namespace thor {

template<typename Base, typename Signature, typename Token>
struct Realization;

template<typename Base, typename... Args, typename Token>
struct Realization<Base, void(Args...), Token> : Base {
	template<typename... CArgs>
	explicit Realization(Token token, CArgs &&... args)
	: Base(frigg::forward<CArgs>(args)...), _completer(frigg::move(token)) { }

	void complete(Args... args) override {
		_completer(frigg::move(args)...);
	}

private:
	typename Token::Completer _completer;
};

struct StreamControl {
	enum {
		kTagNull,
		kTagOffer,
		kTagAccept,
		kTagSendFromBuffer,
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

	explicit OfferBase(LaneDescriptor lane)
	: StreamControl(kTagOffer), _lane(frigg::move(lane)) { }

	virtual void complete(Error error) = 0;

	LaneDescriptor _lane;
};

template<typename Token>
using Offer = Realization<OfferBase, void(Error), Token>;

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

template<typename Token>
using Accept = Realization<
	AcceptBase,
	void(Error, frigg::WeakPtr<Universe>, LaneDescriptor),
	Token
>;

struct SendFromBufferBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagSendFromBuffer;
	}

	explicit SendFromBufferBase(frigg::UniqueMemory<KernelAlloc> buffer)
	: StreamControl(kTagSendFromBuffer), buffer(frigg::move(buffer)) { }

	virtual void complete(Error error) = 0;
	
	frigg::UniqueMemory<KernelAlloc> buffer;
};

template<typename Token>
using SendFromBuffer = Realization<SendFromBufferBase, void(Error), Token>;

struct RecvToBufferBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagRecvToBuffer;
	}

	explicit RecvToBufferBase(ForeignSpaceLock accessor)
	: StreamControl(kTagRecvToBuffer), accessor(frigg::move(accessor)) { }

	virtual void complete(Error error, size_t length) = 0;
	
	ForeignSpaceLock accessor;
};

template<typename Token>
using RecvToBuffer = Realization<RecvToBufferBase, void(Error, size_t), Token>;

struct PushDescriptorBase : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagPushDescriptor;
	}

	explicit PushDescriptorBase(AnyDescriptor lane)
	: StreamControl(kTagPushDescriptor), _lane(frigg::move(lane)) { }

	virtual void complete(Error error) = 0;

	AnyDescriptor _lane;
};

template<typename Token>
using PushDescriptor = Realization<PushDescriptorBase, void(Error), Token>;

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

template<typename Token>
using PullDescriptor = Realization<
	PullDescriptorBase,
	void(Error, frigg::WeakPtr<Universe>, AnyDescriptor),
	Token
>;

struct Stream {
	// manage the peer counter of each lane.
	// incrementing a peer counter that is already at zero is undefined.
	// decrement returns true if the counter reaches zero.
	static void incrementPeers(Stream *stream, int lane);
	static bool decrementPeers(Stream *stream, int lane);

	Stream();
	~Stream();

	// submits an operation to the stream.
	void submit(int lane, frigg::SharedPtr<StreamControl> control);

private:
	std::atomic<int> _peerCount[2];
	
	frigg::TicketLock _mutex;

	// protected by _mutex.
	frigg::IntrusiveSharedLinkedList<
		StreamControl,
		&StreamControl::processQueueItem
	> _processQueue[2];
	
	// protected by _mutex.
	bool _laneBroken[2];
};

} // namespace thor

