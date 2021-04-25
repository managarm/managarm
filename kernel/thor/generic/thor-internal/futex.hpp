#pragma once

#include <async/cancellation.hpp>
#include <frg/functional.hpp>
#include <frg/hash_map.hpp>
#include <frg/list.hpp>
#include <frg/spinlock.hpp>

#include <thor-internal/cancel.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct FutexRealm;

// This struct uniquely identifies a futex.
struct FutexIdentity {
	struct Hash {
		size_t operator() (FutexIdentity id) const {
			auto h = [] (uintptr_t x) -> uintptr_t {
				static_assert(sizeof(uintptr_t) == 8);
				x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
				x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
				x = x ^ (x >> 31);
				return x;
			};

			return 3 * h(reinterpret_cast<uintptr_t>(id.spaceQualifier)) + h(id.localAddress);
		}
	};

	bool operator== (const FutexIdentity &) const = default;

	// Both of these values are opaque to the futex code.
	uintptr_t spaceQualifier = 0;
	uintptr_t localAddress = 0;
};

// This concept allows access to a futex.
// The FutexRealm code calls retire() after it is done with the futex. For example, retire()
// can be used to unpin the memory page that contains the futex.
template<typename F>
concept Futex = requires(F f) {
	// TODO: We would like to enfore return type here but we do not have the <concepts> header
	//       in our current libstdc++ installation.
	f.getIdentity();
	f.read();
	f.retire();
};

struct FutexRealm {
private:
	// Represents a single waiter.
	struct Node {
		friend struct FutexRealm;

		Node(FutexRealm *realm, FutexIdentity id)
		: realm_{realm}, id_{id}, cobs_{this} { }

	protected:
		virtual void complete() = 0;

	private:
		void cancel_() {
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&realm_->_mutex);

				if(!result_) {
					auto sit = realm_->_slots.get(id_);
					// Invariant: If the slot exists then its queue is not empty.
					assert(!sit->queue.empty());

					auto nit = sit->queue.iterator_to(this);
					sit->queue.erase(nit);
					result_ = Error::cancelled;

					if(sit->queue.empty())
						realm_->_slots.remove(id_);
				}else{
					assert(!queueHook_.in_list);
				}
			}

			complete();
		}

		FutexRealm *realm_;
		FutexIdentity id_;
		frg::optional<Error> result_; // Set after completion.
		async::cancellation_observer<frg::bound_mem_fn<&Node::cancel_>> cobs_;
		frg::default_list_hook<Node> queueHook_;
	};

	struct Slot {
		frg::intrusive_list<
			Node,
			frg::locate_member<
				Node,
				frg::default_list_hook<Node>,
				&Node::queueHook_
			>
		> queue;
	};

public:
	FutexRealm()
	: _slots{FutexIdentity::Hash{}, *kernelAlloc} { }

	bool empty() {
		return _slots.empty();
	}

	// ----------------------------------------------------------------------------------
	// wait().
	// ----------------------------------------------------------------------------------

	template<Futex F, typename R>
	struct WaitOperation : private Node {
		WaitOperation(FutexRealm *self, F f, unsigned int expected,
				async::cancellation_token ct, R receiver)
		: Node{self, f.getIdentity()}, f_{std::move(f)}, expected_{expected}, ct_{ct},
				receiver_{std::move(receiver)} { }

		WaitOperation(const WaitOperation &) = delete;

		WaitOperation &operator= (const WaitOperation &) = delete;

		bool start_inline() {
			// We still need the futex after unlocking in the lambda below. However,
			// the operation struct can be deallocated at any time after unlocking.
			// Move the futex to the stack to avoid memory safety issues.
			F f = std::move(f_);

			auto fastPath = [&] {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&realm_->_mutex);

				if(f.read() != expected_) {
					result_ = Error::futexRace;
					return true;
				}

				if(!cobs_.try_set(ct_)) {
					result_ = Error::cancelled;
					return true;
				}

				auto sit = realm_->_slots.get(id_);
				if(!sit) {
					realm_->_slots.insert(id_, Slot());
					sit = realm_->_slots.get(id_);
				}

				assert(!queueHook_.in_list);
				sit->queue.push_back(this);
				return false;
			}(); // Immediately invoked.

			// Retire up the Futex after installing the waiter.
			f.retire();

			if(fastPath) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		F f_;
		unsigned int expected_;
		async::cancellation_token ct_;
		R receiver_;
	};

	template<Futex F>
	struct [[nodiscard]] WaitSender {
		using value_type = void;

		template<typename R>
		WaitOperation<F, R> connect(R receiver) {
			return {self, std::move(f), expected, ct, std::move(receiver)};
		}

		async::sender_awaiter<WaitSender> operator co_await() {
			return {std::move(*this)};
		}

		FutexRealm *self;
		F f;
		unsigned int expected;
		async::cancellation_token ct;
	};

	template<Futex F>
	WaitSender<F> wait(F f, unsigned int expected, async::cancellation_token ct = {}) {
		return {this, std::move(f), expected, ct};
	}

	// ----------------------------------------------------------------------------------

	void wake(FutexIdentity id) {
		frg::intrusive_list<
			Node,
			frg::locate_member<
				Node,
				frg::default_list_hook<Node>,
				&Node::queueHook_
			>
		> pending;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			auto sit = _slots.get(id);
			if(!sit)
				return;
			// Invariant: If the slot exists then its queue is not empty.
			assert(!sit->queue.empty());

			// TODO: Enable users to only wake a certain number of waiters.
			while(!sit->queue.empty()) {
				auto node = sit->queue.front();
				assert(!node->result_);
				sit->queue.pop_front();

				if(node->cobs_.try_reset()) {
					node->result_ = Error::success;
					pending.push_back(node);
				}
			}

			if(sit->queue.empty())
				_slots.remove(id);
		}

		while(!pending.empty()) {
			auto node = pending.pop_front();
			node->complete();
		}
	}

private:
	using Mutex = frg::ticket_spinlock;

	// TODO: use a scalable hash table with fine-grained locks to
	// improve the scalability of the futex algorithm.
	Mutex _mutex;

	frg::hash_map<
		FutexIdentity,
		Slot,
		FutexIdentity::Hash,
		KernelAlloc
	> _slots;
};

} // namespace thor
