
namespace thor {

struct StreamControl {
	enum {
		kTagSendFromBuffer = 1,
		kTagRecvToBuffer
	};

	StreamControl(int tag)
	: _tag(tag) { }

	int tag() const {
		return _tag;
	}

	frigg::IntrusiveSharedLinkedItem<StreamControl> processQueueItem;

private:
	int _tag;
};

struct SendFromBuffer : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagSendFromBuffer;
	}

	SendFromBuffer(frigg::UniqueMemory<KernelAlloc> buffer)
	: StreamControl(kTagSendFromBuffer), buffer(frigg::move(buffer)) { }

	virtual void complete() = 0;
	
	frigg::UniqueMemory<KernelAlloc> buffer;
};

struct RecvToBuffer : StreamControl {
	static bool classOf(const StreamControl &base) {
		return base.tag() == kTagRecvToBuffer;
	}

	RecvToBuffer(ForeignSpaceLock accessor)
	: StreamControl(kTagRecvToBuffer), accessor(frigg::move(accessor)) { }

	virtual void complete() = 0;
	
	ForeignSpaceLock accessor;
};

struct Stream {
	Stream();

	// submits an operation to the stream.
	void submit(int lane, frigg::SharedPtr<StreamControl> control);

	// TODO: this should _really_ be an atomic integer
	int peerCounter[2];

private:
	frigg::IntrusiveSharedLinkedList<
		StreamControl,
		&StreamControl::processQueueItem
	> _processQueue[2];
};

} // namespace thor

