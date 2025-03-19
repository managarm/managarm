#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <iostream>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <core/clock.hpp>
#include <helix/ipc.hpp>
#include "fs.hpp"
#include "timerfd.hpp"

namespace {

bool logTimerfd = false;

uint64_t add_sat(uint64_t x, uint64_t y) {
	uint64_t r;
	if (__builtin_add_overflow(x, y, &r))
		return UINT64_MAX;
	return r;
}

struct OpenFile : File {
private:
	struct Timer {
		uint64_t asyncId;
		uint64_t initial;
		uint64_t interval;
	};
	
	// TODO: Unify this implementation with the itimer implementation in process.hpp.
	async::detached arm(Timer *timer) {
		assert(timer->initial || timer->interval);

		uint64_t tick = timer->initial;

		if(timer->initial) {
			helix::AwaitClock await_initial;
			auto &&submit = helix::submitAwaitClock(&await_initial, tick,
					helix::Dispatcher::global());
			timer->asyncId = await_initial.asyncId();
			co_await submit.async_wait();
			timer->asyncId = 0;
			assert(!await_initial.error() || await_initial.error() == kHelErrCancelled);

			if(_activeTimer == timer) {
				_expirations++;
				_theSeq++;
				_seqBell.raise();
			}else{
				delete timer;
				co_return;
			}
		}

		if(!timer->interval) {
			if(_activeTimer == timer)
				_activeTimer = nullptr;
			delete timer;
			co_return;
		}

		while(true) {
			helix::AwaitClock await_interval;
			auto &&submit = helix::submitAwaitClock(&await_interval, add_sat(tick, timer->interval),
					helix::Dispatcher::global());
			timer->asyncId = await_interval.asyncId();
			co_await submit.async_wait();
			timer->asyncId = 0;
			assert(!await_interval.error() || await_interval.error() == kHelErrCancelled);
			tick = add_sat(tick, timer->interval);

			if(_activeTimer == timer) {
				_expirations++;
				_theSeq++;
				_seqBell.raise();
			}else{
				delete timer;
				co_return;
			}
		}
	}

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(int clock, bool non_block)
	: File{StructName::get("timerfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)},
			_clock{clock}, _nonBlock{non_block},
			_activeTimer{nullptr}, _expirations{0}, _theSeq{0} {
		assert(_clock == CLOCK_MONOTONIC || _clock == CLOCK_REALTIME);
		(void)_nonBlock;
	}

	~OpenFile() override {
		// Nothing to do here.
	}

	void handleClose() override {
		_seqBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		assert(max_length == sizeof(uint64_t));
		assert(_expirations);

		memcpy(data, &_expirations, sizeof(uint64_t));
		_expirations = 0;
		co_return sizeof(uint64_t);
	}
	
	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t in_seq, int mask,
			async::cancellation_token cancellation) override {
		(void)mask; // TODO: utilize mask.
		if(logTimerfd)
			std::cout << "posix: timerfd::pollWait(" << in_seq << ")" << std::endl;
		assert(in_seq <= _theSeq);
		while(in_seq == _theSeq && !cancellation.is_cancellation_requested()) {
			if(!isOpen())
				co_return Error::fileClosed;

			co_await _seqBell.async_wait(cancellation);
		}

		co_return PollWaitResult(_theSeq, _theSeq ? EPOLLIN : 0);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult(_theSeq, _expirations ? EPOLLIN : 0);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void setTime(bool relative, uint64_t initial, uint64_t interval) {
		// TODO: Add a better abstraction for different clocks.
		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		if (initial) {
			if (relative) {
				// Transform relative time to absolute time since boot.
				initial = add_sat(now, initial);
			} else {
				if (_clock == CLOCK_REALTIME) {
					// Transform real time to time since boot.
					int64_t bootTime = clk::getRealtimeNanos() - now;
					assert(bootTime >= 0);
					if (initial < bootTime) {
						// TODO: This is not entirely correct but arm() does not handle negative times.
						initial = 1;
					} else {
						initial -= bootTime;
					}
				}
			}
		}

		auto current = std::exchange(_activeTimer, nullptr);
		if(current) {
			assert(current->asyncId);
			HEL_CHECK(helCancelAsync(helix::Dispatcher::global().acquire(), current->asyncId));
		}

		if(initial || interval) {
			_activeTimer = new Timer;
			_activeTimer->asyncId = 0;
			_activeTimer->initial = initial;
			_activeTimer->interval = interval;
			_expirations = 0;
			arm(_activeTimer);
		}
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
	int _clock;
	bool _nonBlock;

	// Currently active timer.
	Timer *_activeTimer;

	// Number of expirations since last read().
	uint64_t _expirations;

	uint64_t _theSeq;
	async::recurring_event _seqBell;
};

} // anonymous namespace

namespace timerfd {

smarter::shared_ptr<File, FileHandle> createFile(int clock, bool non_block) {
	auto file = smarter::make_shared<OpenFile>(clock, non_block);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

void setTime(File *file, int flags, struct timespec initial, struct timespec interval) {
	assert(initial.tv_sec >= 0 && initial.tv_nsec >= 0);
	assert(interval.tv_sec >= 0 && interval.tv_nsec >= 0);

	if(logTimerfd)
		std::cout << "setTime() initial: " << initial.tv_sec << " + " << initial.tv_nsec
				<< ", interval: " << interval.tv_sec << " + " << interval.tv_nsec << std::endl;

	// Note: __builtin_mul_overflow() with signed arguments requires a call to a
	// compiler-rt function for clang. Cast to unsigned to avoid this issue.

	uint64_t initial_nanos;
	if(__builtin_mul_overflow(static_cast<uint64_t>(initial.tv_sec), 1000000000, &initial_nanos)
			|| __builtin_add_overflow(initial.tv_nsec, initial_nanos, &initial_nanos)) {
		initial_nanos = UINT64_MAX;
	}

	uint64_t interval_nanos;
	if(__builtin_mul_overflow(static_cast<uint64_t>(interval.tv_sec), 1000000000, &interval_nanos)
			|| __builtin_add_overflow(interval.tv_nsec, interval_nanos, &interval_nanos))
		interval_nanos = UINT64_MAX;

	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->setTime(!(flags & TFD_TIMER_ABSTIME), initial_nanos, interval_nanos);
}

} // namespace timerfd

