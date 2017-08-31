
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

	frg::default_list_hook<QueueNode> _queueNode;
};

struct QueueSpace {
	using Address = uintptr_t;

public:
	QueueSpace()
	: _slots(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	void submit(frigg::UnsafePtr<AddressSpace> space, Address address, QueueNode *element);

private:
	using Mutex = frigg::TicketLock;

	struct Slot : FutexNode {
		Slot(QueueSpace *manager, frigg::UnsafePtr<AddressSpace> space,
				Address address)
		: manager{manager}, space{space}, address{address} { }

		void onWake() override;

		QueueSpace *manager;
		frigg::UnsafePtr<AddressSpace> space;
		Address address;

		frg::intrusive_list<
			QueueNode,
			frg::locate_member<
				QueueNode,
				frg::default_list_hook<QueueNode>,
				&QueueNode::_queueNode
			>
		> queue;
	};
	
	using NodeList = frg::intrusive_list<
		QueueNode,
		frg::locate_member<
			QueueNode,
			frg::default_list_hook<QueueNode>,
			&QueueNode::_queueNode
		>
	>;

	void _progress(Slot *slot);
	bool _progressFront(Slot *slot, Address &successor, NodeList &migrate_list);

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

} // namespace thor

