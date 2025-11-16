#include "common.hpp"
#include <sys/time.h>
#include <sys/timerfd.h>

namespace requests {

// SET_INTERVAL_TIMER handler (setitimer)
async::result<void> handleSetIntervalTimer(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetIntervalTimerRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	managarm::posix::SetIntervalTimerResponse resp;
	if(req->which() == ITIMER_REAL) {
		logRequest(logRequests, ctx, "SETITIMER", "value={}.{:06d}s interval={}.{:06d}s",
			req->value_sec(), req->value_usec(), req->interval_sec(), req->interval_usec());

		uint64_t value = 0;
		uint64_t interval = 0;
		if(ctx.self->threadGroup()->realTimer)
			ctx.self->threadGroup()->realTimer->getTime(value, interval);

		resp.set_value_sec(value / 1'000'000'000);
		resp.set_value_usec((value % 1'000'000'000) / 1'000);
		resp.set_interval_sec(interval / 1'000'000'000);
		resp.set_interval_usec((interval % 1'000'000'000) / 1'000);

		if(ctx.self->threadGroup()->realTimer)
			ctx.self->threadGroup()->realTimer->cancel();

		if (req->value_sec() || req->value_usec()) {
			auto valueNanos = posix::convertToNanos(
				timespec{static_cast<time_t>(req->value_sec()), static_cast<long>(req->value_usec() * 1000)},
				CLOCK_REALTIME, true);
			auto intervalNanos = posix::convertToNanos(
				timespec{static_cast<time_t>(req->interval_sec()), static_cast<long>(req->interval_usec() * 1000)},
				CLOCK_MONOTONIC);

			ctx.self->threadGroup()->realTimer = std::make_shared<ThreadGroup::IntervalTimer>(ctx.self,
					valueNanos, intervalNanos);
			ctx.self->threadGroup()->realTimer->arm(ctx.self->threadGroup()->realTimer);
		}

		resp.set_error(managarm::posix::Errors::SUCCESS);
	} else {
		// TODO: handle ITIMER_VIRTUAL and ITIMER_PROF
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		std::cout << "posix: ITIMER_VIRTUAL and ITIMER_PROF are unsupported" << std::endl;
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// TIMER_CREATE handler
async::result<void> handleTimerCreate(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerCreateRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_CREATE", "clockid={}", req->clockid());

	managarm::posix::TimerCreateResponse resp;
	if(req->clockid() == CLOCK_MONOTONIC || req->clockid() == CLOCK_REALTIME) {
		auto id = ctx.self->threadGroup()->timerIdAllocator.allocate();
		assert(!ctx.self->threadGroup()->timers.contains(id));

		ctx.self->threadGroup()->timers[id] = std::make_shared<ThreadGroup::PosixTimerContext>(
			req->clockid(),
			nullptr,
			req->sigev_tid() ? std::optional{req->sigev_tid()} : std::nullopt,
			req->sigev_signo()
		);

		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_timer_id(id);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		std::println("posix: unsupported clock_id {}", req->clockid());
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// TIMER_SET handler
async::result<void> handleTimerSet(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerSetRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_SET", "timer={}", req->timer());

	managarm::posix::TimerSetResponse resp;
	resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

	if(ctx.self->threadGroup()->timers.contains(req->timer())) {
		auto timerContext = ctx.self->threadGroup()->timers[req->timer()];

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

		auto targetThread = ctx.self;
		if(timerContext->tid && targetThread->tid() != *timerContext->tid)
			targetThread = ctx.self->threadGroup()->findThread(*timerContext->tid);

		if(targetThread) {
			uint64_t valueNanos = 0;
			uint64_t intervalNanos = 0;

			if(req->value_sec() || req->value_nsec()) {
				valueNanos = posix::convertToNanos(
					{static_cast<time_t>(req->value_sec()), static_cast<long>(req->value_nsec())},
					timerContext->clockid, !(req->flags() & TFD_TIMER_ABSTIME));
				intervalNanos = posix::convertToNanos(
					{static_cast<time_t>(req->interval_sec()), static_cast<long>(req->interval_nsec())},
					CLOCK_MONOTONIC);
			}

			timerContext->timer = std::make_shared<ThreadGroup::PosixTimer>(targetThread,
				timerContext->tid, timerContext->signo, req->timer(), valueNanos, intervalNanos);
			timerContext->timer->arm(timerContext->timer);
			resp.set_error(managarm::posix::Errors::SUCCESS);
		}
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// TIMER_GET handler
async::result<void> handleTimerGet(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerGetRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_GET", "timer={}", req->timer());

	managarm::posix::TimerGetResponse resp;
	if(ctx.self->threadGroup()->timers.contains(req->timer())) {
		auto timerContext = ctx.self->threadGroup()->timers[req->timer()];
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
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// TIMER_DELETE handler
async::result<void> handleTimerDelete(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerDeleteRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_DELETE", "timer={}", req->timer());

	managarm::posix::TimerDeleteResponse resp;
	if(ctx.self->threadGroup()->timers.contains(req->timer())) {
		auto timerCtx = ctx.self->threadGroup()->timers[req->timer()];
		if(timerCtx->timer) {
			timerCtx->timer->cancel();
			timerCtx->timer = nullptr;
		}
		ctx.self->threadGroup()->timers.erase(req->timer());
		ctx.self->threadGroup()->timerIdAllocator.free(req->timer());
		resp.set_error(managarm::posix::Errors::SUCCESS);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
