#include "common.hpp"
#include <sys/time.h>
#include <sys/timerfd.h>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetIntervalTimerRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	managarm::posix::SetIntervalTimerResponse resp;
	if(req.which() == ITIMER_REAL) {
		logRequest(logRequests, self, "SETITIMER", "value={}.{:06d}s interval={}.{:06d}s",
			req.value_sec(), req.value_usec(), req.interval_sec(), req.interval_usec());

		uint64_t value = 0;
		uint64_t interval = 0;
		if(self->threadGroup()->realTimer)
			self->threadGroup()->realTimer->getTime(value, interval);

		resp.set_value_sec(value / 1'000'000'000);
		resp.set_value_usec((value % 1'000'000'000) / 1'000);
		resp.set_interval_sec(interval / 1'000'000'000);
		resp.set_interval_usec((interval % 1'000'000'000) / 1'000);

		if(self->threadGroup()->realTimer)
			self->threadGroup()->realTimer->cancel();

		if (req.value_sec() || req.value_usec()) {
			auto valueNanos = posix::convertToNanos(
				timespec{static_cast<time_t>(req.value_sec()), static_cast<long>(req.value_usec() * 1000)},
				CLOCK_REALTIME, true);
			auto intervalNanos = posix::convertToNanos(
				timespec{static_cast<time_t>(req.interval_sec()), static_cast<long>(req.interval_usec() * 1000)},
				CLOCK_MONOTONIC);

			self->threadGroup()->realTimer = std::make_shared<ThreadGroup::IntervalTimer>(self,
					valueNanos, intervalNanos);
			self->threadGroup()->realTimer->arm(self->threadGroup()->realTimer);
		}

		resp.set_error(managarm::posix::Errors::SUCCESS);
	} else {
		// TODO: handle ITIMER_VIRTUAL and ITIMER_PROF
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		std::cout << "posix: ITIMER_VIRTUAL and ITIMER_PROF are unsupported" << std::endl;
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_CREATE", "clockid={}", req.clockid());

	managarm::posix::TimerCreateResponse resp;
	if(req.clockid() == CLOCK_MONOTONIC || req.clockid() == CLOCK_REALTIME) {
		auto timerId = self->threadGroup()->timerIdAllocator.allocate();
		assert(!self->threadGroup()->timers.contains(timerId));

		self->threadGroup()->timers[timerId] = std::make_shared<ThreadGroup::PosixTimerContext>(
			req.clockid(),
			nullptr,
			req.sigev_tid() ? std::optional{req.sigev_tid()} : std::nullopt,
			req.sigev_signo()
		);

		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_timer_id(timerId);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		std::println("posix: unsupported clock_id {}", req.clockid());
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerSetRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_SET", "timer={}", req.timer());

	managarm::posix::TimerSetResponse resp;
	resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

	if(self->threadGroup()->timers.contains(req.timer())) {
		auto timerContext = self->threadGroup()->timers[req.timer()];

		uint64_t value = 0;
		uint64_t interval = 0;
		if(timerContext->timer)
			timerContext->timer->getTime(value, interval);

		resp.set_value_sec(value / 1'000'000'000);
		resp.set_value_nsec(value % 1'000'000'000);
		resp.set_interval_sec(interval / 1'000'000'000);
		resp.set_interval_nsec(interval % 1'000'000'000);

		if(timerContext->timer)
			timerContext->timer->cancel();

		auto targetThread = self;
		if(timerContext->tid && targetThread->tid() != *timerContext->tid)
			targetThread = self->threadGroup()->findThread(*timerContext->tid);

		if(targetThread) {
			uint64_t valueNanos = 0;
			uint64_t intervalNanos = 0;

			if(req.value_sec() || req.value_nsec()) {
				valueNanos = posix::convertToNanos(
					{static_cast<time_t>(req.value_sec()), static_cast<long>(req.value_nsec())},
					timerContext->clockid, !(req.flags() & TFD_TIMER_ABSTIME));
				intervalNanos = posix::convertToNanos(
					{static_cast<time_t>(req.interval_sec()), static_cast<long>(req.interval_nsec())},
					CLOCK_MONOTONIC);
			}

			timerContext->timer = std::make_shared<ThreadGroup::PosixTimer>(targetThread,
				timerContext->tid, timerContext->signo, req.timer(), valueNanos, intervalNanos);
			timerContext->timer->arm(timerContext->timer);
			resp.set_error(managarm::posix::Errors::SUCCESS);
		}
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerGetRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_GET", "timer={}", req.timer());

	managarm::posix::TimerGetResponse resp;
	if(self->threadGroup()->timers.contains(req.timer())) {
		auto timerContext = self->threadGroup()->timers[req.timer()];
		resp.set_error(managarm::posix::Errors::SUCCESS);

		uint64_t value = 0;
		uint64_t interval = 0;
		if(timerContext->timer)
			timerContext->timer->getTime(value, interval);

		resp.set_value_sec(value / 1'000'000'000);
		resp.set_value_nsec(value % 1'000'000'000);
		resp.set_interval_sec(interval / 1'000'000'000);
		resp.set_interval_nsec(interval % 1'000'000'000);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerDeleteRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_DELETE", "timer={}", req.timer());

	managarm::posix::TimerDeleteResponse resp;
	if(self->threadGroup()->timers.contains(req.timer())) {
		auto timerCtx = self->threadGroup()->timers[req.timer()];
		if(timerCtx->timer) {
			timerCtx->timer->cancel();
			timerCtx->timer = nullptr;
		}
		self->threadGroup()->timers.erase(req.timer());
		self->threadGroup()->timerIdAllocator.free(req.timer());
		resp.set_error(managarm::posix::Errors::SUCCESS);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
