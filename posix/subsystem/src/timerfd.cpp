// needed to avoid redefinitions in linux/timerfd.h
#ifdef _GNU_SOURCE
#undef _GNU_SOURCE
#endif

#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <print>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <core/clock.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <protocols/fs/common.hpp>

#include "clocks.hpp"
#include "fs.hpp"
#include "interval-timer.hpp"
#include "protocols/fs/common.hpp"
#include "timerfd.hpp"

// avoid flock redefinition
#define HAVE_ARCH_STRUCT_FLOCK
#include <linux/timerfd.h>

namespace {

bool logTimerfd = false;

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

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ct) override {
		if(max_length < sizeof(uint64_t))
			co_return std::unexpected{Error::illegalArguments};

		if(!_expirations && nonBlock_)
			co_return std::unexpected{Error::wouldBlock};

		while(!_expirations) {
			if (!co_await _seqBell.async_wait(ct))
				co_return std::unexpected{Error::interrupted};
		}

		memcpy(data, &_expirations, sizeof(uint64_t));
		_expirations = 0;
		co_return sizeof(uint64_t);
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t in_seq, int mask,
			async::cancellation_token cancellation) override {
		if(logTimerfd)
			std::cout << "posix: timerfd::pollWait(" << in_seq << ")" << std::endl;

		assert(in_seq <= _theSeq);

		int edges = 0;
		while(true) {
			if(!isOpen())
				co_return Error::fileClosed;

			edges = 0;
			if (_theSeq > in_seq)
				edges |= EPOLLIN;

			if (edges & mask)
				break;

			if (!co_await _seqBell.async_wait(cancellation))
				break;
		}

		co_return PollWaitResult(_theSeq, edges & mask);
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

	async::result<void> ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		if(id == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
			if (!req) {
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				co_return;
			}

			switch(req->command()) {
				case TFD_IOC_SET_TICKS: {
					_expirations = req->ticks();
					_theSeq++;
					_seqBell.raise();

					managarm::fs::GenericIoctlReply resp;
					resp.set_error(managarm::fs::Errors::SUCCESS);

					auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
					HEL_CHECK(send_resp.error());
					break;
				}
				default: {
					std::println("timerfd: unexpected ioctl request 0x{:x}", req->command());
					auto [dismiss] = co_await helix_ng::exchangeMsgs(
						conversation, helix_ng::dismiss());
					HEL_CHECK(dismiss.error());
					break;
				}
			}
		} else {
			std::println("timerfd: unexpected ioctl message type 0x{:x}\n", id);
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void setTime(bool relative, const timespec initial, const timespec interval) {
		uint64_t initialNanos = 0;
		uint64_t intervalNanos = 0;

		if(initial.tv_sec || initial.tv_nsec) {
			initialNanos = posix::convertToNanos(initial, _clock, relative);
			intervalNanos = posix::convertToNanos(interval, CLOCK_MONOTONIC);
		}

		if(_activeTimer)
			_activeTimer->cancel();
		if(initialNanos || intervalNanos) {
			_activeTimer = std::make_shared<Timer>(weakFile(), initialNanos, intervalNanos);
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
	if(logTimerfd)
		std::cout << "setTime() initial: " << initial.tv_sec << " + " << initial.tv_nsec
				<< ", interval: " << interval.tv_sec << " + " << interval.tv_nsec << std::endl;

	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->setTime(!(flags & TFD_TIMER_ABSTIME), initial, interval);
}

void getTime(File *file, timespec &initial, timespec &interval) {
	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->getTime(initial, interval);
}

} // namespace timerfd

