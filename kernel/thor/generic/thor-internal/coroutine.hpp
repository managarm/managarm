#pragma once

#include <async/basic.hpp>
#include <thor-internal/debug.hpp>

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
			return {std::experimental::coroutine_handle<promise_type>::from_promise(*this)};
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

				void await_suspend(std::experimental::coroutine_handle<void>) {
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

				void await_suspend(std::experimental::coroutine_handle<void>) noexcept {
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

	coroutine(std::experimental::coroutine_handle<promise_type> h)
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
	std::experimental::coroutine_handle<promise_type> h_;
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
			return {std::experimental::coroutine_handle<promise_type>::from_promise(*this)};
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

				void await_suspend(std::experimental::coroutine_handle<void>) {
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

				void await_suspend(std::experimental::coroutine_handle<void>) noexcept {
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

	coroutine(std::experimental::coroutine_handle<promise_type> h)
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
	std::experimental::coroutine_handle<promise_type> h_;
};

template<typename T, typename R>
struct coroutine_operation final : private coroutine_continuation<T> {
	coroutine_operation(coroutine<T> s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	coroutine_operation(const coroutine_operation &) = delete;

	coroutine_operation &operator= (const coroutine_operation &) = delete;

	bool start_inline() {
		auto h = s_.h_;
		auto promise = &h.promise();
		promise->cont_ = this;
		h.resume();
		auto cfp = promise->cfp_.exchange(coroutine_cfp::past_start, std::memory_order_relaxed);
		if(cfp == coroutine_cfp::past_suspend) {
			// Synchronize with the thread that complete the coroutine.
			std::atomic_thread_fence(std::memory_order_acquire);
			async::execution::set_value_inline(receiver_, std::move(value()));
			return true;
		}
		return false;
	}

private:
	void resume() override {
		async::execution::set_value_noinline(receiver_, std::move(value()));
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

	bool start_inline() {
		auto h = s_.h_;
		auto promise = &h.promise();
		promise->cont_ = this;
		h.resume();
		auto cfp = promise->cfp_.exchange(coroutine_cfp::past_start, std::memory_order_relaxed);
		if(cfp == coroutine_cfp::past_suspend) {
			// Synchronize with the thread that complete the coroutine.
			std::atomic_thread_fence(std::memory_order_acquire);
			async::execution::set_value_inline(receiver_);
			return true;
		}
		return false;
	}

private:
	void resume() override {
		async::execution::set_value_noinline(receiver_);
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
