#pragma once

#include <async/cancellation.hpp>
#include <async/oneshot-event.hpp>
#include <frg/functional.hpp>
#include <frg/hash_map.hpp>
#include <frg/list.hpp>
#include <frg/spinlock.hpp>

#include <thor-internal/cancel.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/work-queue.hpp>

#include <thor-internal/debug.hpp>

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

template<typename F>
concept Futex = requires(F f) {
	f.getIdentity();
	f.read();
};

template<typename S>
concept FutexSpace = requires(S s) {
	// Provides temporary access to a Futex.
	{ s.withFutex(uintptr_t{}, [] (Futex auto) {}) } -> std::same_as<coroutine<frg::expected<Error>>>;
};

struct FutexRealm {
private:
	enum class State {
		none,
		done,
		cancelled,
	};

	// Represents a single waiter.
	struct Node {
		State st{State::none};
		frg::default_list_hook<Node> queueHook;
		async::oneshot_primitive completionEvent;
	};

	struct Slot {
		frg::intrusive_list<
			Node,
			frg::locate_member<
				Node,
				frg::default_list_hook<Node>,
				&Node::queueHook
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

	template<FutexSpace S>
	coroutine<Error> wait(S space, uintptr_t address, unsigned int expected,
			async::cancellation_token ct = {}) {
		Node node{};
		FutexIdentity id;

		bool futexRace = false;
		auto result = co_await space.withFutex(address, [&](auto futex) {
			id = futex.getIdentity();

			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			if(futex.read() != expected) {
				futexRace = true;
				return;
			}

			auto sit = _slots.get(id);
			if(!sit) {
				_slots.insert(id, Slot());
				sit = _slots.get(id);
			}

			sit->queue.push_back(&node);
		});
		if(!result)
			co_return result.error();
		if (futexRace)
			co_return Error::futexRace;

		co_await async::with_cancel_cb(
			node.completionEvent.wait(),
			[&] {
				// Remove the node from the futex's wait list.
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&_mutex);

					if (node.st == State::done)
						return;
					assert(node.st == State::none);

					auto sit = _slots.get(id);
					assert(sit);

					// Invariant: If the slot exists then its queue is not empty.
					assert(!sit->queue.empty());

					auto nit = sit->queue.iterator_to(&node);
					sit->queue.erase(nit);
					node.st = State::cancelled;

					if(sit->queue.empty())
						_slots.remove(id);
				}

				node.completionEvent.raise();
			},
			ct
		);
		if (node.st == State::done) {
			co_return Error::success;
		} else {
			assert(node.st == State::cancelled);
			co_return Error::cancelled;
		}
	}

	// ----------------------------------------------------------------------------------
	// wake().
	// ----------------------------------------------------------------------------------

	template<FutexSpace S>
	coroutine<frg::expected<Error>> wake(S space, uintptr_t address, uint32_t count) {
		FutexIdentity id;

		auto result = co_await space.withFutex(address, [&](auto futex) {
			id = futex.getIdentity();
		});

		if(!result)
			co_return result.error();

		frg::intrusive_list<
			Node,
			frg::locate_member<
				Node,
				frg::default_list_hook<Node>,
				&Node::queueHook
			>
		> pending;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			auto sit = _slots.get(id);
			if(!sit)
				co_return {};
			// Invariant: If the slot exists then its queue is not empty.
			assert(!sit->queue.empty());

			while(!sit->queue.empty() && count) {
				auto node = sit->queue.front();
				assert(node->st == State::none);
				sit->queue.pop_front();

				node->st = State::done;
				pending.push_back(node);

				count--;
			}

			if(sit->queue.empty())
				_slots.remove(id);
		}

		while(!pending.empty()) {
			auto node = pending.pop_front();
			node->completionEvent.raise();
		}

		co_return {};
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
