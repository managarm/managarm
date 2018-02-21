
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include "timerfd.hpp"

namespace {

bool logTimerfd = false;

struct OpenFile : ProxyFile {
private:
	struct Timer {
		uint64_t initial;
		uint64_t interval;
	};
	
	COFIBER_ROUTINE(cofiber::no_future, arm(Timer *timer), ([=] {
		assert(timer->initial || timer->interval);
//		std::cout << "posix: Timer armed" << std::endl;

		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

		if(timer->initial) {
			helix::AwaitClock await_initial;
			auto &&submit = helix::submitAwaitClock(&await_initial, tick + timer->initial,
					helix::Dispatcher::global());
			COFIBER_AWAIT submit.async_wait();
			HEL_CHECK(await_initial.error());
			tick += timer->initial;

			if(_activeTimer == timer) {
				_expirations++;
				_theSeq++;
				_seqBell.ring();
			}else{
				COFIBER_RETURN();
			}
		}

		if(!timer->interval)
			COFIBER_RETURN();

		while(true) {
			helix::AwaitClock await_interval;
			auto &&submit = helix::submitAwaitClock(&await_interval, tick + timer->interval,
					helix::Dispatcher::global());
			COFIBER_AWAIT submit.async_wait();
			HEL_CHECK(await_interval.error());
			tick += timer->interval;

			if(_activeTimer == timer) {
				_expirations++;
				_theSeq++;
				_seqBell.ring();
			}else{
				COFIBER_RETURN();
			}
		}
	}))

	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static async::result<size_t> ptRead(std::shared_ptr<void> object,
			void *buffer, size_t length) {
		auto self = static_cast<OpenFile *>(object.get());
		return self->readSome(buffer, length);
	}
	
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withRead(&ptRead);

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	OpenFile()
	: ProxyFile{nullptr}, _activeTimer{nullptr}, _expirations{0}, _theSeq{1} { }

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(max_length == sizeof(uint64_t));
		assert(_expirations);

		memcpy(data, &_expirations, sizeof(uint64_t));
		_expirations = 0;
		COFIBER_RETURN(sizeof(uint64_t));
	}))
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t in_seq) override, ([=] {
		if(logTimerfd)
			std::cout << "posix: timerfd::poll(" << in_seq << ")" << std::endl;
		assert(in_seq <= _theSeq);
		while(in_seq == _theSeq)
			COFIBER_AWAIT _seqBell.async_wait();

		COFIBER_RETURN(PollResult(_theSeq, EPOLLIN, _expirations ? EPOLLIN : 0));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void setTime(uint64_t initial, uint64_t interval) {
		if(initial || interval) {
			_activeTimer = new Timer;
			_activeTimer->initial = initial;
			_activeTimer->interval = interval;
			arm(_activeTimer);
		}else{
			_activeTimer = nullptr;
		}
	}

private:
	helix::UniqueLane _passthrough;

	// Currently active timer.
	Timer *_activeTimer;

	// Number of expirations since last read().
	uint64_t _expirations;

	uint64_t _theSeq;
	async::doorbell _seqBell;
};

} // anonymous namespace

namespace timerfd {

std::shared_ptr<ProxyFile> createFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

void setTime(File *file, struct timespec initial, struct timespec interval) {
	assert(initial.tv_sec >= 0 && initial.tv_nsec >= 0);
	assert(interval.tv_sec >= 0 && interval.tv_nsec >= 0);

	if(logTimerfd)
		std::cout << "setTime() initial: " << initial.tv_sec << " + " << initial.tv_nsec
				<< ", interval: " << interval.tv_sec << " + " << interval.tv_nsec << std::endl;

	uint64_t initial_nanos;
	if(__builtin_mul_overflow(initial.tv_sec, 1000000000, &initial_nanos)
			|| __builtin_add_overflow(initial.tv_nsec, initial_nanos, &initial_nanos))
		throw std::runtime_error("Overflow in timerfd setup");

	uint64_t interval_nanos;
	if(__builtin_mul_overflow(interval.tv_sec, 1000000000, &interval_nanos)
			|| __builtin_add_overflow(interval.tv_nsec, interval_nanos, &interval_nanos))
		throw std::runtime_error("Overflow in timerfd setup");

	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->setTime(initial_nanos, interval_nanos);
}

} // namespace timerfd

