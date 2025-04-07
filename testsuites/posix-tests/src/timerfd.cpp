#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(timerfd, ([] {
	int t = timerfd_create(CLOCK_MONOTONIC, 0);
	assert(t > 0);

	struct itimerspec its;
	int ret = timerfd_gettime(t, &its);
	assert(ret == 0);
	assert(!its.it_value.tv_sec);
	assert(!its.it_value.tv_nsec);
	assert(!its.it_interval.tv_sec);
	assert(!its.it_interval.tv_nsec);

	struct itimerspec new_its {
		.it_interval = {0, 0},
		.it_value = {0, 100'000'000},
	};
	ret = timerfd_settime(t, 0, &new_its, &its);
	assert(ret == 0);
	assert(!its.it_value.tv_sec);
	assert(!its.it_value.tv_nsec);
	assert(!its.it_interval.tv_sec);
	assert(!its.it_interval.tv_nsec);

	ret = timerfd_gettime(t, &its);
	assert(ret == 0);
	assert(!its.it_value.tv_sec);
	assert(its.it_value.tv_nsec);
	assert(!its.it_interval.tv_sec);
	assert(!its.it_interval.tv_nsec);

	struct timeval before;
	gettimeofday(&before, nullptr);
	assert(ret == 0);
	uint64_t ev = 0;
	ret = read(t, &ev, sizeof(ev));
	struct timeval after;
	gettimeofday(&after, nullptr);

	assert(ret == sizeof(ev));
	assert(ev == 1);

	struct timeval diff = {};
	timersub(&after, &before, &diff);
	assert(diff.tv_sec || diff.tv_usec);

	fcntl(t, F_SETFL, fcntl(t, F_GETFL, 0) | O_NONBLOCK);

	ret = timerfd_settime(t, 0, &new_its, &its);
	assert(ret == 0);

	ret = read(t, &ev, sizeof(ev));
	assert(ret == -1);
	assert(errno == EAGAIN);
}));
