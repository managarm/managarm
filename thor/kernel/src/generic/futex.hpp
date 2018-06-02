#ifndef THOR_GENERIC_FUTEX_HPP
#define THOR_GENERIC_FUTEX_HPP

#include <frg/list.hpp>
#include <frigg/atomic.hpp>
#include <frigg/linked.hpp>
#include <frigg/hashmap.hpp>
#include "cancel.hpp"
#include "kernel_heap.hpp"
#include "../arch/x86/ints.hpp"

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

		// TODO: Enable users to only wake a certain number of waiters.
		wake_queue.splice(wake_queue.end(), it->queue);
		
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

} // namespace thor

#endif // THOR_GENERIC_FUTEX_HPP
