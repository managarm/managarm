#ifndef THOR_GENERIC_IPCQUEUE_HPP
#define THOR_GENERIC_IPCQUEUE_HPP

#include <frg/list.hpp>
#include "accessors.hpp"
#include "cancel.hpp"
#include "kernel_heap.hpp"
#include "../arch/x86/ints.hpp"

namespace thor {

struct IpcQueue;

// NOTE: The following structs mirror the Hel{Queue,Element} structs.
// They must be kept in sync!

static const int kHeadMask = 0xFFFFFF;
static const int kHeadWaiters = (1 << 24);

struct QueueStruct {
	int headFutex;
	unsigned int elementLimit;
	unsigned int sizeShift;
	char padding[4];
	int indexQueue[];
};

static const int kProgressMask = 0xFFFFFF;
static const int kProgressWaiters = (1 << 24);
static const int kProgressDone = (1 << 25);

struct ChunkStruct {
	int progressFutex;
	char padding[4];
	char buffer[];
};

struct ElementStruct {
	unsigned int length;
	unsigned int reserved;
	void *context;
};

struct QueueSource {
	void setup(void *pointer_, size_t size_) {
		pointer = pointer_;
		size = size_;
	}

	void *pointer;
	size_t size;
	const QueueSource *link;
};

struct IpcNode {
	friend struct IpcQueue;

	IpcNode()
	: _context{0}, _source{nullptr} { }

	// Users of IpcQueue::submit() have to set this up first.
	void setupContext(uintptr_t context) {
		_context = context;
	}
	void setupSource(QueueSource *source) {
		_source = source;
	}

	virtual void complete() = 0;
	
private:
	uintptr_t _context;
	const QueueSource *_source;

	IpcQueue *_queue;
	Worklet _worklet;
	frg::default_list_hook<IpcNode> _queueNode;
};

struct IpcQueue : CancelRegistry {
private:
	using Address = uintptr_t;

	using NodeList = frg::intrusive_list<
		IpcNode,
		frg::locate_member<
			IpcNode,
			frg::default_list_hook<IpcNode>,
			&IpcNode::_queueNode
		>
	>;

	using Mutex = frigg::TicketLock;

	struct Chunk {
		Chunk()
		: pointer{nullptr} { }

		Chunk(smarter::shared_ptr<AddressSpace, BindableHandle> space_, void *pointer_)
		: space{frigg::move(space_)}, pointer{pointer_}, bufferSize{4096} { }

		// Pointer (+ address space) to queue chunk struct.
		smarter::shared_ptr<AddressSpace, BindableHandle> space;
		void *pointer;

		// Size of the chunk's buffer.
		size_t bufferSize;
	};

public:
	IpcQueue(smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer);

	IpcQueue(const IpcQueue &) = delete;

	IpcQueue &operator= (const IpcQueue &) = delete;

	bool validSize(size_t size);

	void setupChunk(size_t index, smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer);

	void submit(IpcNode *node);

private:
	void _progress();
	void _advanceChunk();
	void _retireChunk();
	bool _waitHeadFutex();
	void _wakeProgressFutex(bool done);

private:	
	Mutex _mutex;

	// Pointer (+ address space) to queue head struct.
	smarter::shared_ptr<AddressSpace, BindableHandle> _space;
	void *_pointer;
	
	int _sizeShift;

	Worklet _worklet;
	AcquireNode _acquireNode;
	FutexNode _futex;

	// Accessors for the queue header.
	AddressSpaceLockHandle _queuePin;
	DirectSpaceAccessor<QueueStruct> _queueAccessor;

	bool _waitInFutex;

	// Points to the chunk that we're currently writing.
	Chunk *_currentChunk;

	// Accessors for the current chunk.
	AddressSpaceLockHandle _chunkPin;
	DirectSpaceAccessor<ChunkStruct> _chunkAccessor;

	// Progress into the current chunk.
	int _currentProgress;

	// Index into the queue that we're currently processing.
	int _nextIndex;

	frigg::Vector<Chunk, KernelAlloc> _chunks;

	frg::intrusive_list<
		IpcNode,
		frg::locate_member<
			IpcNode,
			frg::default_list_hook<IpcNode>,
			&IpcNode::_queueNode
		>
	> _nodeQueue;
};

} // namespace thor

#endif // THOR_GENERIC_IPCQUEUE_HPP
