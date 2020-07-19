#pragma once

#include <async/cancellation.hpp>
#include <frg/functional.hpp>
#include <frg/hash_map.hpp>
#include <frg/list.hpp>
#include <frigg/atomic.hpp>
#include <frigg/linked.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/cancel.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct FutexSpace;

enum class FutexState {
	none,
	waiting,
	woken,
	retired
};

struct FutexNode {
	friend struct FutexSpace;

	FutexNode()
	: _cancelCb{this} { }

	void setup(Worklet *woken) {
		_woken = woken;
	}

private:
	void onCancel();

	FutexSpace *_futex = nullptr;
	AddressIdentity _identity;
	Worklet *_woken;
	async::cancellation_token _cancellation;
	FutexState _state = FutexState::none;
	bool _wasCancelled = false;
	async::cancellation_observer<frg::bound_mem_fn<&FutexNode::onCancel>> _cancelCb;
	frg::default_list_hook<FutexNode> _queueNode;
};

struct FutexSpace {
	friend struct FutexNode;

	FutexSpace()
	: _slots{AddressIdentityHash{}, *kernelAlloc} { }

	bool empty() {
		return _slots.empty();
	}

	template<typename C>
	bool checkSubmitWait(AddressIdentity identity, C condition, FutexNode *node,
			async::cancellation_token cancellation = {}) {
		// TODO: avoid reuse of FutexNode and remove this condition.
		if(node->_state == FutexState::retired) {
			node->_futex = nullptr;
			node->_state = FutexState::none;
		}
		assert(!node->_futex);
		node->_futex = this;
		node->_identity = identity;
		node->_cancellation = cancellation;

		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);
		assert(node->_state == FutexState::none);

		if(!condition()) {
			node->_state = FutexState::retired;
			return false;
		}
		if(!node->_cancelCb.try_set(node->_cancellation)) {
			node->_wasCancelled = true;
			node->_state = FutexState::retired;
			return false;
		}

		auto sit = _slots.get(identity);
		if(!sit) {
			_slots.insert(identity, Slot());
			sit = _slots.get(identity);
		}

		assert(!node->_queueNode.in_list);
		sit->queue.push_back(node);
		node->_state = FutexState::waiting;
		return true;
	}

	template<typename C>
	void submitWait(AddressIdentity identity, C condition, FutexNode *node) {
		if(!checkSubmitWait(identity, std::move(condition), node))
			WorkQueue::post(node->_woken);
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for wait()
	// ----------------------------------------------------------------------------------

	template<typename R, typename Condition>
	struct WaitOperation;

	template<typename Condition>
	struct [[nodiscard]] WaitSender {
		using value_type = void;

		template<typename R>
		friend WaitOperation<R, Condition>
		connect(WaitSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		FutexSpace *self;
		AddressIdentity identity;
		Condition c;
		async::cancellation_token cancellation;
	};

	template<typename Condition>
	WaitSender<Condition> wait(AddressIdentity identity, Condition c,
			async::cancellation_token cancellation = {}) {
		return {this, identity, std::move(c), cancellation};
	}

	template<typename R, typename Condition>
	struct WaitOperation {
		WaitOperation(WaitSender<Condition> s, R receiver)
		: s_{std::move(s)}, receiver_{std::move(receiver)} { }

		WaitOperation(const WaitOperation &) = delete;

		WaitOperation &operator= (const WaitOperation &) = delete;

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &WaitOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_);
			});
			node_.setup(&worklet_);
			if(!s_.self->checkSubmitWait(s_.identity, std::move(s_.c), &node_, s_.cancellation)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		WaitSender<Condition> s_;
		R receiver_;
		FutexNode node_;
		Worklet worklet_;
	};

	template<typename Condition>
	friend async::sender_awaiter<WaitSender<Condition>>
	operator co_await(WaitSender<Condition> sender) {
		return {std::move(sender)};
	}

	// ----------------------------------------------------------------------------------

private:
	void cancel(FutexNode *node) {
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if(node->_state == FutexState::waiting) {
			auto sit = _slots.get(node->_identity);
			// Invariant: If the slot exists then its queue is not empty.
			assert(!sit->queue.empty());

			auto nit = sit->queue.iterator_to(node);
			sit->queue.erase(nit);
			node->_wasCancelled = true;

			if(sit->queue.empty())
				_slots.remove(node->_identity);
		}else{
			// Let the cancellation handler invoke the continuation.
			assert(node->_state == FutexState::woken);
		}

		node->_state = FutexState::retired;
		WorkQueue::post(node->_woken);
	}

public:
	void wake(AddressIdentity identity) {
		frg::intrusive_list<
			FutexNode,
			frg::locate_member<
				FutexNode,
				frg::default_list_hook<FutexNode>,
				&FutexNode::_queueNode
			>
		> pending;
		{
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&_mutex);

			auto sit = _slots.get(identity);
			if(!sit)
				return;
			// Invariant: If the slot exists then its queue is not empty.
			assert(!sit->queue.empty());

			// TODO: Enable users to only wake a certain number of waiters.
			while(!sit->queue.empty()) {
				auto node = sit->queue.front();
				assert(node->_state == FutexState::waiting);
				sit->queue.pop_front();

				if(node->_cancelCb.try_reset()) {
					node->_state = FutexState::retired;
					pending.push_back(node);
				}else{
					node->_state = FutexState::woken;
				}
			}

			if(sit->queue.empty())
				_slots.remove(identity);
		}

		while(!pending.empty()) {
			auto node = pending.pop_front();
			WorkQueue::post(node->_woken);
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

	frg::hash_map<
		AddressIdentity,
		Slot,
		AddressIdentityHash,
		KernelAlloc
	> _slots;
};

inline void FutexNode::onCancel() {
	assert(_futex);
	_futex->cancel(this);
}

FutexSpace *getGlobalFutexSpace();

} // namespace thor
