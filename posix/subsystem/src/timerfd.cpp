#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <print>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <core/clock.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>

#include "fs.hpp"
#include "interval-timer.hpp"
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
	struct Timer : posix::IntervalTimer {
		Timer(smarter::weak_ptr<File> file, uint64_t initial, uint64_t interval)
		: IntervalTimer{initial, interval}, file_{file} {
			assert(file_.lock()->kind() == FileKind::timerfd);
		}

		void raise(bool success) override {
			auto f = smarter::static_pointer_cast<OpenFile>(file_.lock());
			if(!f || !f->_activeTimer || f->_activeTimer.get() != this)
				return;

			if(success) {
				f->_expirations++;
				f->_theSeq++;
				f->_seqBell.raise();
			}
		}

		void expired() override {
			auto f = smarter::static_pointer_cast<OpenFile>(file_.lock());

			if(!f || f->_activeTimer.get() != this)
				return;

			f->_activeTimer = nullptr;
		}

	private:
		smarter::weak_ptr<File> file_;
	};

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(int clock, bool non_block)
	: File{FileKind::timerfd,  StructName::get("timerfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)},
			_clock{clock}, nonBlock_{non_block},
			_activeTimer{nullptr}, _expirations{0}, _theSeq{0} {
		assert(_clock == CLOCK_MONOTONIC || _clock == CLOCK_REALTIME);
	}

	~OpenFile() override {
		// Nothing to do here.
	}

	void handleClose() override {
		if(_activeTimer)
			_activeTimer.reset();
		_seqBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		if(max_length < sizeof(uint64_t))
			co_return Error::illegalArguments;

		if(!_expirations && nonBlock_)
			co_return Error::wouldBlock;

		while(!_expirations)
			co_await _seqBell.async_wait();

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

	async::result<int> getFileFlags() override {
		int flags = 0;

		if(nonBlock_)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~O_NONBLOCK) {
			std::println("posix: setFileFlags on \e[1;34m{}\e[0m called with unknown flags {:#x}",
				structName(), flags & ~O_NONBLOCK);
			co_return;
		}

		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
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
					if (initial < static_cast<uint64_t>(bootTime)) {
						// TODO: This is not entirely correct but arm() does not handle negative times.
						initial = 1;
					} else {
						initial -= bootTime;
					}
				}
			}
		}

		if(_activeTimer)
			_activeTimer->cancel();
		if(initial || interval) {
			_activeTimer = std::make_shared<Timer>(weakFile(), initial, interval);
			_expirations = 0;
			Timer::arm(_activeTimer);
		} else {
			// disarm timer
			_activeTimer = nullptr;
		}
	}

	void getTime(timespec &initial, timespec &interval) {
		if(_activeTimer) {
			uint64_t initialNanos = 0;
			uint64_t intervalNanos = 0;

			_activeTimer->getTime(initialNanos, intervalNanos);

			initial.tv_sec = initialNanos / 1'000'000'000;
			initial.tv_nsec = initialNanos % 1'000'000'000;
			interval.tv_sec = intervalNanos / 1'000'000'000;
			interval.tv_nsec = intervalNanos % 1'000'000'000;
		} else {
			initial.tv_sec = 0;
			initial.tv_nsec = 0;
			interval.tv_sec = 0;
			interval.tv_nsec = 0;
		}
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
	int _clock;
	bool nonBlock_;

	// Currently active timer.
	std::shared_ptr<Timer> _activeTimer;

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

void getTime(File *file, timespec &initial, timespec &interval) {
	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->getTime(initial, interval);
}

} // namespace timerfd

