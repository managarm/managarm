
#ifndef HELIX_AWAIT_HPP
#define HELIX_AWAIT_HPP

#include <atomic>

#include <cofiber.hpp>

namespace helix {

struct AwaitMechanism {
	enum {
		is_complete = 1,
		is_awaiting = 2
	};

	struct Completer;

	struct Future {
		friend class AwaitMechanism;

		bool await_ready() {
			// TODO: this could be optimized.
			return false;
		}

		void await_suspend(cofiber::coroutine_handle<> handle) {
			_completer->_handle = handle;
			auto s = _completer->_status.fetch_or(is_awaiting, std::memory_order_acq_rel);
			assert(!(s & is_awaiting));
			if(s & is_complete)
				handle.resume();
		}

		void await_resume() {
			// just return void here.
		}

	private:
		Future(Completer *completer)
		: _completer(completer) { }

		Completer *_completer;
	};

	struct Completer {
		friend class AwaitMechanism;

		Completer()
		: _status(0) { }

		Completer(const Completer &) = delete;
		Completer(Completer &&) = delete;

		Completer &operator= (Completer) = delete;

		Future future() {
			return Future(this);
		}

		void operator() () {
			auto s = _status.fetch_or(is_complete, std::memory_order_acq_rel);
			assert(!(s & is_complete));
			if(s & is_awaiting)
				_handle.resume();
		}

	private:
		std::atomic<int> _status;
		cofiber::coroutine_handle<> _handle;
	};
};

} // namespace helix

#endif // HELIX_AWAIT_HPP

