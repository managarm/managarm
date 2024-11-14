#pragma once

#include <frg/list.hpp>
#include <frg/vector.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cancel.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/mm-rc.hpp>

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

protected:
	~IpcNode() = default;

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

	using Mutex = frg::ticket_spinlock;

public:
	IpcQueue(unsigned int ringShift, unsigned int numChunks, size_t chunkSize);

	IpcQueue(const IpcQueue &) = delete;

	IpcQueue &operator= (const IpcQueue &) = delete;

	smarter::shared_ptr<MemoryView> getMemory() {
		return _memory;
	}

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
	struct SubmitOperation final : private IpcNode {
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
			async::execution::set_value(receiver_);
		}

		SubmitSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<SubmitSender>
	operator co_await(SubmitSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

private:
	coroutine<void> _runQueue();

private:
	Mutex _mutex;

	smarter::shared_ptr<ImmediateMemory> _memory;

	unsigned int _ringShift;
	size_t _chunkSize;

	frg::vector<size_t, KernelAlloc> _chunkOffsets;

	// Index into the queue that we are currently processing.
	int _currentIndex;
	// Progress into the current chunk.
	int _currentProgress;

	async::recurring_event _doorbell;

	frg::intrusive_list<
		IpcNode,
		frg::locate_member<
			IpcNode,
			frg::default_list_hook<IpcNode>,
			&IpcNode::_queueNode
		>
	> _nodeQueue;

	// Stores whether any nodes are in the queue.
	// Written only when _mutex is held (but read outside of _mutex).
	std::atomic<bool> _anyNodes;
};

} // namespace thor
