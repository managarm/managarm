#pragma once

#include <async/basic.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

template<async::Sender S>
struct WorkQueueAffineAwaiter : Worklet {
	struct Receiver {
		template<typename... Args>
		void set_value(Args &&... args) {
			aw->value_.emplace(std::forward<Args>(args)...);
			aw->setup([] (Worklet *base) {
				auto aw = static_cast<WorkQueueAffineAwaiter *>(base);
				aw->h_.resume();
			});
			aw->wq_->post(aw);
		}

		WorkQueueAffineAwaiter *aw;
	};

	WorkQueueAffineAwaiter(S s, WorkQueue *wq)
	: op_{async::execution::connect(std::move(s), Receiver{.aw = this})},
		wq_{wq} { }

	bool await_ready() { return false; }

	void await_suspend(std::coroutine_handle<void> h) {
		h_ = h;
		async::execution::start(op_);
	}

	typename S::value_type await_resume() {
		assert(value_);
		if constexpr (!std::is_same_v<typename S::value_type, void>)
			return std::move(*value_);
	}

	async::execution::operation_t<S, Receiver> op_;
	WorkQueue *wq_;
	std::coroutine_handle<void> h_;

	struct empty { };

	std::optional<
		std::conditional_t<
			std::is_same_v<typename S::value_type, void>,
			empty,
			typename S::value_type
		>
	> value_;
};

} // namespace thor

template<typename T>
struct coroutine_continuation {
	void pass_value(T value) {
		obj_.emplace(std::move(value));
	}

	virtual void resume() = 0;

protected:
	T &value() {
		return *obj_;
	}

	~coroutine_continuation() = default;

private:
	frg::optional<T> obj_;
};

// Specialization for coroutines without results.
template<>
struct coroutine_continuation<void> {
	virtual void resume() = 0;

protected:
	~coroutine_continuation() = default;
};

// "Control flow path" that the coroutine takes. This state is used to distinguish inline
// completion from asynchronous completion. In contrast to other state machines, the states
// are not significant only their own; only transitions matter:
// On past_suspend -> past_start transitions, we continue inline.
// On past_start -> past_suspend transitions, we call resume().
enum class coroutine_cfp {
	indeterminate,
	past_start, // We are past start_inline().
	past_suspend // We are past final_suspend.
};

template<typename T, typename R>
struct coroutine_operation;

template<typename T>
struct coroutine {
	template<typename T_, typename R>
	friend struct coroutine_operation;

	using value_type = T;

	struct promise_type {
		template<typename T_, typename R>
		friend struct coroutine_operation;

		void *operator new(size_t size) {
			return thor::kernelAlloc->allocate(size);
		}

		void operator delete(void *p, size_t size) {
			return thor::kernelAlloc->deallocate(p, size);
		}

		coroutine get_return_object() {
			return {std::coroutine_handle<promise_type>::from_promise(*this)};
		}

		void unhandled_exception() {
			thor::panicLogger() << "thor: Unhandled exception in coroutine<T>" << frg::endlog;
		}

		void return_value(T value) {
			cont_->pass_value(std::move(value));
		}

		auto initial_suspend() {
			struct awaiter {
				awaiter(promise_type *promise)
				: promise_{promise} { }

				bool await_ready() {
					return false;
				}

				void await_suspend(std::coroutine_handle<void>) {
					// Do nothing.
				}

				void await_resume() {
					assert(promise_->cont_);
				}

			private:
				promise_type *promise_;
			};
			return awaiter{this};
		}

		auto final_suspend() noexcept {
			struct awaiter {
				awaiter(promise_type *promise)
				: promise_{promise} { }

				bool await_ready() noexcept {
					return false;
				}

				void await_suspend(std::coroutine_handle<void>) noexcept {
					auto cfp = promise_->cfp_.exchange(coroutine_cfp::past_suspend,
							std::memory_order_release);
					if(cfp == coroutine_cfp::past_start) {
						// We do not need to synchronize with the thread that started the
						// coroutine here, as that thread is already done on its part.
						promise_->cont_->resume();
					}
				}

				void await_resume() noexcept {
					__builtin_trap();
				}

			private:
				promise_type *promise_;
			};
			return awaiter{this};
		}

	private:
		coroutine_continuation<T> *cont_ = nullptr;
		std::atomic<coroutine_cfp> cfp_{coroutine_cfp::indeterminate};
	};

	coroutine()
	: h_{} { }

	coroutine(std::coroutine_handle<promise_type> h)
	: h_{h} { }

	coroutine(const coroutine &) = delete;

	coroutine(coroutine &&other)
	: coroutine{} {
		std::swap(h_, other.h_);
	}

	~coroutine() {
		if(h_)
			h_.destroy();
	}

	coroutine &operator= (coroutine other) {
		std::swap(h_, other.h_);
		return *this;
	}

private:
	std::coroutine_handle<promise_type> h_;
};


// Specialization for coroutines without results.
template<>
struct coroutine<void> {
	template<typename T_, typename R>
	friend struct coroutine_operation;

	using value_type = void;

	struct promise_type {
		template<typename T_, typename R>
		friend struct coroutine_operation;

		void *operator new(size_t size) {
			return thor::kernelAlloc->allocate(size);
		}

		void operator delete(void *p, size_t size) {
			return thor::kernelAlloc->deallocate(p, size);
		}

		coroutine get_return_object() {
			return {std::coroutine_handle<promise_type>::from_promise(*this)};
		}

		void unhandled_exception() {
			thor::panicLogger() << "thor: Unhandled exception in coroutine<T>" << frg::endlog;
		}

		void return_void() {
			// Do nothing.
		}

		auto initial_suspend() {
			struct awaiter {
				awaiter(promise_type *promise)
				: promise_{promise} { }

				bool await_ready() {
					return false;
				}

				void await_suspend(std::coroutine_handle<void>) {
					// Do nothing.
				}

				void await_resume() {
					assert(promise_->cont_);
				}

			private:
				promise_type *promise_;
			};
			return awaiter{this};
		}

		auto final_suspend() noexcept {
			struct awaiter {
				awaiter(promise_type *promise)
				: promise_{promise} { }

				bool await_ready() noexcept {
					return false;
				}

				void await_suspend(std::coroutine_handle<void>) noexcept {
					auto cfp = promise_->cfp_.exchange(coroutine_cfp::past_suspend,
							std::memory_order_release);
					if(cfp == coroutine_cfp::past_start) {
						// We do not need to synchronize with the thread that started the
						// coroutine here, as that thread is already done on its part.
						promise_->cont_->resume();
					}
				}

				void await_resume() noexcept {
					__builtin_trap();
				}

			private:
				promise_type *promise_;
			};
			return awaiter{this};
		}

	private:
		coroutine_continuation<void> *cont_ = nullptr;
		std::atomic<coroutine_cfp> cfp_{coroutine_cfp::indeterminate};
	};

	coroutine()
	: h_{} { }

	coroutine(std::coroutine_handle<promise_type> h)
	: h_{h} { }

	coroutine(const coroutine &) = delete;

	coroutine(coroutine &&other)
	: coroutine{} {
		std::swap(h_, other.h_);
	}

	~coroutine() {
		if(h_)
			h_.destroy();
	}

	coroutine &operator= (coroutine other) {
		std::swap(h_, other.h_);
		return *this;
	}

private:
	std::coroutine_handle<promise_type> h_;
};

template<typename T, typename R>
struct coroutine_operation final : private coroutine_continuation<T> {
	coroutine_operation(coroutine<T> s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	coroutine_operation(const coroutine_operation &) = delete;

	coroutine_operation &operator= (const coroutine_operation &) = delete;

	void start() {
		auto h = s_.h_;
		auto promise = &h.promise();
		promise->cont_ = this;
		h.resume();
		auto cfp = promise->cfp_.exchange(coroutine_cfp::past_start, std::memory_order_relaxed);
		if(cfp == coroutine_cfp::past_suspend) {
			// Synchronize with the thread that complete the coroutine.
			std::atomic_thread_fence(std::memory_order_acquire);
			return async::execution::set_value(receiver_, std::move(value()));
		}
	}

private:
	void resume() override {
		async::execution::set_value(receiver_, std::move(value()));
	}

private:
	using coroutine_continuation<T>::value;

	coroutine<T> s_;
	R receiver_;
};

// Specialization for coroutines without results.
template<typename R>
struct coroutine_operation<void, R> final : private coroutine_continuation<void> {
	coroutine_operation(coroutine<void> s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	coroutine_operation(const coroutine_operation &) = delete;

	coroutine_operation &operator= (const coroutine_operation &) = delete;

	void start() {
		auto h = s_.h_;
		auto promise = &h.promise();
		promise->cont_ = this;
		h.resume();
		auto cfp = promise->cfp_.exchange(coroutine_cfp::past_start, std::memory_order_relaxed);
		if(cfp == coroutine_cfp::past_suspend) {
			// Synchronize with the thread that complete the coroutine.
			std::atomic_thread_fence(std::memory_order_acquire);
			return async::execution::set_value(receiver_);
		}
	}

private:
	void resume() override {
		async::execution::set_value(receiver_);
	}

private:
	coroutine<void> s_;
	R receiver_;
};

template<typename T, typename R>
coroutine_operation<T, R> connect(coroutine<T> s, R receiver) {
	return {std::move(s), std::move(receiver)};
};

template<typename T>
async::sender_awaiter<coroutine<T>, T> operator co_await(coroutine<T> s) {
	return {std::move(s)};
}

namespace detail {
	template<typename T>
	constexpr T &lastArg(T &t) { return t; }

	template<typename Head, typename... Tail>
	constexpr auto &lastArg(Head &, Tail &... tail) { return lastArg(tail...); }
}

// Helper type that marks void-returning functions as detached coroutines.
// Must be passed as last argument to the function.
// Example usage: [] (enable_detached_coroutine) -> void { co_await foobar(); }
struct enable_detached_coroutine {
	smarter::shared_ptr<thor::WorkQueue> wq;
};

struct detached_coroutine_promise {
	void *operator new(size_t size) {
		return thor::kernelAlloc->allocate(size);
	}

	void operator delete(void *p, size_t size) {
		return thor::kernelAlloc->deallocate(p, size);
	}

	template<typename... Args>
	detached_coroutine_promise(Args &... args)
	: wq_{detail::lastArg(args...).wq.get()} { }

	void get_return_object() {
		// Our return object is void.
	}

	void unhandled_exception() {
		thor::panicLogger() << "thor: Unhandled exception in detached coroutine" << frg::endlog;
	}

	void return_void() {
		// Do nothing.
	}

	auto initial_suspend() {
		return std::suspend_never{};
	}

	auto final_suspend() noexcept {
		return std::suspend_never{};
	}

	template<async::Sender S>
	auto await_transform(S &&s) {
		return thor::WorkQueueAffineAwaiter{std::move(s), wq_};
	}

private:
	thor::WorkQueue *wq_;
};

template<typename... Ts>
struct last_type {
    using type = typename decltype((std::type_identity<Ts>{}, ...))::type;
};

template<typename... Ts>
using last_type_t = typename last_type<Ts...>::type;

namespace std {
	template<typename... Args>
	requires (std::is_same_v<last_type_t<Args...>, enable_detached_coroutine>)
	struct coroutine_traits<void, Args...> {
		using promise_type = detached_coroutine_promise;
	};

	template<typename X, typename... Args>
	requires (std::is_same_v<last_type_t<Args...>, enable_detached_coroutine>)
	struct coroutine_traits<void, X, Args...> {
		using promise_type = detached_coroutine_promise;
	};
}

namespace thor {

template<typename A, async::Sender S>
struct WqSpawnCtrlBlock {
	struct Receiver {
		void set_value() {
			A allocator = std::move(cb->allocator_);
			frg::destruct(allocator, cb);
		}

		WqSpawnCtrlBlock *cb;
	};

	WqSpawnCtrlBlock(A allocator, smarter::shared_ptr<WorkQueue> wq, S sender)
	: allocator_{std::move(allocator)},
		wq_{std::move(wq)},
		op_{async::execution::connect(std::move(sender), Receiver{.cb = this})} { }

	void spawn() {
		op_.start();
	}

private:
	A allocator_;
	smarter::shared_ptr<WorkQueue> wq_;
	async::execution::operation_t<S, Receiver> op_;
};

template<typename A, async::Sender S>
void spawnOnWorkQueue(A allocator, smarter::shared_ptr<WorkQueue> wq, S sender) {
	auto cb = frg::construct<WqSpawnCtrlBlock<A, S>>(allocator, allocator, std::move(wq), std::move(sender));
	cb->spawn();
}

} // namespace thor
