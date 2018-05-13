
#include <frg/list.hpp>
#include <frigg/linked.hpp>
#include <frigg/hashmap.hpp>
#include "accessors.hpp"
#include "kernel_heap.hpp"

namespace thor {

struct FutexNode {
	friend struct Futex;

	virtual void onWake() = 0;

private:
	frg::default_list_hook<FutexNode> _queueNode;
};

struct Futex {
	using Address = uintptr_t;

	Futex()
	: _slots(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	bool empty() {
		return _slots.empty();
	}
	
	template<typename C>
	bool checkSubmitWait(Address address, C condition, FutexNode *node) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if(!condition())
			return false;

		auto it = _slots.get(address);
		if(!it) {
			_slots.insert(address, Slot());
			it = _slots.get(address);
		}

		assert(!node->_queueNode.in_list);
		it->queue.push_back(node);
		return true;
	}

	template<typename C>
	void submitWait(Address address, C condition, FutexNode *node) {
		if(!checkSubmitWait(address, std::move(condition), node))
			node->onWake();
	}

	void wake(Address address) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		auto it = _slots.get(address);
		if(!it)
			return;
		
		// Invariant: If the slot exists then its queue is not empty.
		assert(!it->queue.empty());

		// Note: We have to run the onWake() callbacks with locks released.
		// This improves latency and prevents deadlocks if onWake() calls submitWait().
		frg::intrusive_list<
			FutexNode,
			frg::locate_member<
				FutexNode,
				frg::default_list_hook<FutexNode>,
				&FutexNode::_queueNode
			>
		> wake_queue;

		// TODO: Use a splice() call here.
		// TODO: Enable users to only wake a certain number of waiters.
		while(!it->queue.empty())
			wake_queue.push_front(it->queue.pop_front());
		
		_slots.remove(address);

		lock.unlock();
		irq_lock.unlock();

		while(!wake_queue.empty()) {
			auto waiter = wake_queue.pop_front();
			waiter->onWake();
		}
	}

private:	
	using Mutex = frigg::TicketLock;

	struct Slot {
		frg::intrusive_list<
			FutexNode,
			frg::locate_member<
				FutexNode,
				frg::default_list_hook<FutexNode>,
				&FutexNode::_queueNode
			>
		> queue;
	};

	// TODO: use a scalable hash table with fine-grained locks to
	// improve the scalability of the futex algorithm.
	Mutex _mutex;

	frigg::Hashmap<
		Address,
		Slot,
		frigg::DefaultHasher<Address>,
		KernelAlloc
	> _slots;
};

struct QueueChunk {
	void *pointer;
	size_t size;
	const QueueChunk *link;
};

struct QueueNode {
	friend struct UserQueue;

	QueueNode()
	: _context{0}, _chunk{nullptr} { }

	// Users of UserQueue::submit() have to set this up first.
	void setupContext(uintptr_t context) {
		_context = context;
	}
	void setupChunk(QueueChunk *chunk) {
		_chunk = chunk;
	}

	virtual void complete();
	
private:
	uintptr_t _context;
	const QueueChunk *_chunk;

	frg::default_list_hook<QueueNode> _queueNode;
};

struct UserQueue : private FutexNode {
private:
	using Address = uintptr_t;

	using NodeList = frg::intrusive_list<
		QueueNode,
		frg::locate_member<
			QueueNode,
			frg::default_list_hook<QueueNode>,
			&QueueNode::_queueNode
		>
	>;

	using Mutex = frigg::TicketLock;

public:
	UserQueue(frigg::SharedPtr<AddressSpace> space, void *head);

	UserQueue(const UserQueue &) = delete;

	UserQueue &operator= (const UserQueue &) = delete;

	void submit(QueueNode *node);

private:
	void onWake() override;

	void _progress();
	bool _progressFront(Address &successor);

private:
	frigg::SharedPtr<AddressSpace> _space;
	void *_head;

	Mutex _mutex;

	bool _waitInFutex;

	frg::intrusive_list<
		QueueNode,
		frg::locate_member<
			QueueNode,
			frg::default_list_hook<QueueNode>,
			&QueueNode::_queueNode
		>
	> _nodeQueue;
};

} // namespace thor

