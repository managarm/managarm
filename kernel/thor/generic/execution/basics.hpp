#pragma once

#include <experimental/coroutine>
#include <type_traits>

#include <frg/allocation.hpp>
#include <frg/optional.hpp>
#include "../kernel_heap.hpp"

namespace execution {
    struct connect_functor {
        template<typename S, typename R>
        auto operator() (S sender, R receiver) const {
            return connect(std::move(sender), std::move(receiver));
        }
    };

	inline connect_functor connect;

    template<typename S, typename R>
    using operation_t = std::invoke_result_t<connect_functor, S, R>;

	template<typename S, typename T = void>
	struct sender_awaiter {
	private:
		struct receiver {
			void set_done(T result) {
				p_->result_ = std::move(result);
				p_->h_.resume();
			}

			sender_awaiter *p_;
		};

	public:
		sender_awaiter(S sender)
		: operation_{execution::connect(std::move(sender), receiver{this})} {
		}

		bool await_ready() {
			return false;
		}

		void await_suspend(std::experimental::coroutine_handle<> h) {
			h_ = h;
			operation_.start();
		}

		T await_resume() {
			return std::move(*result_);
		}

		execution::operation_t<S, receiver> operation_;
		std::experimental::coroutine_handle<> h_;
		frg::optional<T> result_;
	};

	// Specialization of sender_awaiter for void return types.
	template<typename S>
	struct sender_awaiter<S, void> {
	private:
		struct receiver {
			void set_done() {
				p_->h_.resume();
			}

			sender_awaiter *p_;
		};

	public:
		sender_awaiter(S sender)
		: operation_{execution::connect(std::move(sender), receiver{this})} {
		}

		bool await_ready() {
			return false;
		}

		void await_suspend(std::experimental::coroutine_handle<> h) {
			h_ = h;
			operation_.start();
		}

		void await_resume() {
			// Do nothing.
		}

		execution::operation_t<S, receiver> operation_;
		std::experimental::coroutine_handle<> h_;
	};

	namespace _detach_details {
		template<typename S>
		struct control_block;

		template<typename S>
		void finalize(control_block<S> *cb);

		template<typename S>
		struct receiver {
			receiver(control_block<S> *cb)
			: cb_{cb} { }

			void set_done() {
				finalize(cb_);
			}

		private:
			control_block<S> *cb_;
		};

		// Heap-allocate data structure that holds the operation.
		// We cannot directly put the operation onto the heap as it is non-movable.
		template<typename S>
		struct control_block {
			friend void finalize(control_block<S> *cb) {
				frg::destruct(*thor::kernelAlloc, cb);
			}

			control_block(S sender)
			: operation{connect(std::move(sender), receiver<S>{this})} { }

			operation_t<S, receiver<S>> operation;
		};

		template<typename S>
		void detach(S sender) {
			auto p = frg::construct<control_block<S>>(*thor::kernelAlloc, std::move(sender));
			p->operation.start();
		}
	}

	using _detach_details::detach;
}
