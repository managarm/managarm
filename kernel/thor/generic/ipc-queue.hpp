#ifndef THOR_GENERIC_IPCQUEUE_HPP
#define THOR_GENERIC_IPCQUEUE_HPP

#include <frigg/vector.hpp>
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
	IpcQueue(smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer,
			unsigned int size_shift, size_t element_limit);

	IpcQueue(const IpcQueue &) = delete;

	IpcQueue &operator= (const IpcQueue &) = delete;

	bool validSize(size_t size);

	void setupChunk(size_t index, smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer);

	void submit(IpcNode *node);

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for submit()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SubmitOperation;

	struct [[nodiscard]] SubmitSender {
		template<typename R>
		friend SubmitOperation<R>
		connect(SubmitSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		IpcQueue *self;
		QueueSource *source;
		uintptr_t context;
	};

	SubmitSender submit(QueueSource *source, uintptr_t context) {
		return {this, source, context};
	}

	template<typename R>
	struct SubmitOperation : private IpcNode {
		SubmitOperation(SubmitSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		SubmitOperation(const SubmitOperation &) = delete;

		SubmitOperation &operator= (const SubmitOperation &) = delete;

		void start() {
			setupSource(s_.source);
			setupContext(s_.context);
			s_.self->submit(this);
		}

	private:
		void complete() override {
			receiver_.set_done();
		}

		SubmitSender s_;
		R receiver_;
	};

	friend execution::sender_awaiter<SubmitSender>
	operator co_await(SubmitSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

private:
	void _progress();
	bool _advanceChunk();
	bool _waitHeadFutex();
	void _wakeProgressFutex(bool done);

private:
	Mutex _mutex;

	// Pointer (+ address space) to queue head struct.
	smarter::shared_ptr<AddressSpace, BindableHandle> _space;
	void *_pointer;

	unsigned int _sizeShift;

	Worklet _worklet;
	AcquireNode _acquireNode;
	FutexNode _futex;

	// Accessor for the queue header.
	AddressSpaceLockHandle _queueLock;

	// Index into the queue that we're currently processing.
	int _nextIndex;

	bool _inProgressLoop = false;

	// Points to the chunk that we're currently writing.
	Chunk *_currentChunk;
	// Accessor for the current chunk.
	AddressSpaceLockHandle _chunkLock;
	// Progress into the current chunk.
	int _currentProgress;

	// Accessor for the current element.
	AddressSpaceLockHandle _elementLock;

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
