
#include <frigg/linked.hpp>
#include <frigg/hashmap.hpp>
#include "accessors.hpp"
#include "kernel_heap.hpp"

namespace thor {

struct Futex {
	using Address = uintptr_t;

	Futex()
	: _slots(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	bool empty() {
		return _slots.empty();
	}

	template<typename C, typename F>
	void waitIf(Address address, C condition, F functor) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if(!condition()) {
			functor();
			return;
		}

		auto it = _slots.get(address);
		if(!it) {
			_slots.insert(address, Slot());
			it = _slots.get(address);
		}
		
		it->queue.addBack(frigg::makeShared<Waiter<F>>(*kernelAlloc, frigg::move(functor)));
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
			auto waiter = it->queue.removeFront();
			waiter->complete();
		}
		_slots.remove(address);
	}

private:	
	using Mutex = frigg::TicketLock;

	struct BaseWaiter {
		virtual void complete() = 0;
	
		frigg::IntrusiveSharedLinkedItem<BaseWaiter> hook;
	};

	template<typename F>
	struct Waiter : BaseWaiter {
		Waiter(F functor)
		: _functor(frigg::move(functor)) { }

		void complete() override {
			_functor();
		}

	private:
		F _functor;
	};

	struct Slot {
		// TODO: we do not actually need shared pointers here.
		frigg::IntrusiveSharedLinkedList<
			BaseWaiter,
			&BaseWaiter::hook
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

struct QueueElement {
	friend struct QueueSpace;

	QueueElement()
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

	frigg::IntrusiveSharedLinkedItem<QueueElement> _hook;
};

struct QueueSpace {
	using Address = uintptr_t;

public:
	QueueSpace()
	: _queues(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	void submit(frigg::UnsafePtr<AddressSpace> space, Address address, QueueElement *element);

private:
	using Mutex = frigg::TicketLock;

	struct Queue {
		Queue();

		size_t offset;

		// TODO: we do not actually need shared pointers here.
		frigg::IntrusiveSharedLinkedList<
			QueueElement,
			&QueueElement::_hook
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

