
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
	void submitWait(Address address, C condition, FutexNode *node) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if(!condition()) {
			node->onWake();
			return;
		}

		auto it = _slots.get(address);
		if(!it) {
			_slots.insert(address, Slot());
			it = _slots.get(address);
		}
		
		it->queue.push_back(node);
	}

	void wake(Address address) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		auto it = _slots.get(address);
		if(!it)
			return;
		
		// Invariant: If the slot exists then its queue is not empty.
		assert(!it->queue.empty());

		// TODO: enable users to only wake a certain number of waiters.
		while(!it->queue.empty()) {
			auto waiter = it->queue.pop_front();
			waiter->onWake();
		}
		_slots.remove(address);
	}

private:	
	using Mutex = frigg::TicketLock;

	struct Slot {
		// TODO: we do not actually need shared pointers here.
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

struct QueueNode {
	friend struct QueueSpace;

	QueueNode()
	: _length{0}, _context{0} { }

	// Users of QueueSpace::submit() have to set this up first.
	void setupLength(size_t length) {
		_length = length;
	}
	void setupContext(uintptr_t context) {
		_context = context;
	}
	
	virtual void emit(ForeignSpaceAccessor accessor) = 0;

private:
	size_t _length;
	uintptr_t _context;

	frigg::IntrusiveSharedLinkedItem<QueueNode> _hook;
};

struct QueueSpace {
	using Address = uintptr_t;

public:
	QueueSpace()
	: _queues(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	void submit(frigg::UnsafePtr<AddressSpace> space, Address address, QueueNode *element);

private:
	using Mutex = frigg::TicketLock;

	struct Queue {
		Queue();

		size_t offset;

		// TODO: we do not actually need shared pointers here.
		frigg::IntrusiveSharedLinkedList<
			QueueNode,
			&QueueNode::_hook
		> elements;
	};

	// TODO: use a scalable hash table with fine-grained locks to
	// improve the scalability of the futex algorithm.
	Mutex _mutex;

	frigg::Hashmap<
		Address,
		Queue,
		frigg::DefaultHasher<Address>,
		KernelAlloc
	> _queues;
};

} // namespace thor

