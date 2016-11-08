
namespace thor {

struct Futex {
	using Address = uintptr_t;

	Futex()
	: _slots(frigg::DefaultHasher<Address>(), *kernelAlloc) { }

	template<typename C, typename F>
	void waitIf(Address address, C condition, F functor) {
		auto lock = frigg::guard(&_mutex);
		if(!condition())
			return;

		auto it = _slots.get(address);
		if(!it) {
			_slots.insert(address, Slot());
			it = _slots.get(address);
		}
		
		it->queue.addBack(frigg::makeShared<Waiter<F>>(*kernelAlloc, frigg::move(functor)));
	}

	void wake(Address address) {
		auto lock = frigg::guard(&_mutex);

		// Invariant: If the slot exists then its queue is not empty.
		auto it = _slots.get(address);
		if(!it)
			return;

		auto waiter = it->queue.removeFront();
		if(it->queue.empty())
			_slots.remove(address);

		waiter->complete();
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

} // namespace thor

