#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

#include "testsuite.hpp"

volatile sig_atomic_t sigalarm_flag = 0;
timer_t timerid;

static void sigalarm_handler(int signo, siginfo_t *info, void *) {
	assert(signo == SIGALRM);
	assert(info);
	assert(info->si_signo == SIGALRM);
	assert(info->si_code == SI_TIMER);

	sigalarm_flag = 1;
}

DEFINE_TEST(posix_timers_signal, ([] {
	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = sigalarm_handler;
	sigemptyset(&sa.sa_mask);
	int ret = sigaction(SIGALRM, &sa, nullptr);
	assert(ret == 0);

	ret = timer_create(CLOCK_MONOTONIC, nullptr, &timerid);
	assert(ret == 0);

	struct itimerspec its{
		.it_interval = {
			.tv_sec = 1,
			.tv_nsec = 0,
		},
		.it_value = {
			.tv_sec = 1,
			.tv_nsec = 0,
		},
	};
	ret = timer_settime(timerid, 0, &its, nullptr);
	assert(ret == 0);

	ret = timer_gettime(timerid, &its);
	assert(ret == 0);
	assert(its.it_value.tv_sec || its.it_value.tv_nsec);
	assert(its.it_interval.tv_sec  == 1);

	while(!sigalarm_flag) {
		// Wait for the signal to be delivered.
		pause();
	}

	ret = timer_delete(timerid);
	assert(ret == 0);
}));

constexpr size_t COUNTER_EXPIRATIONS = 3;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
size_t count = 0;

void timer_handler(union sigval) {
	pthread_mutex_lock(&mutex);
	fprintf(stderr, "expiration %zu\n", count);
	count++;
	if(count >= COUNTER_EXPIRATIONS) {
		pthread_cond_signal(&cond);
	}
	pthread_mutex_unlock(&mutex);
}

DEFINE_TEST(posix_timers_sigev_thread, ([] {
	timer_t timer;
	struct sigevent sev{
		.sigev_value = {
			.sival_ptr = reinterpret_cast<void *>(&timer),
		},
		.sigev_notify = SIGEV_THREAD,
	};
	sev.sigev_notify_function = timer_handler;
	sev.sigev_notify_attributes = nullptr;

	int ret = timer_create(CLOCK_MONOTONIC, &sev, &timer);
	assert(ret == 0);

	struct timeval before;
	gettimeofday(&before, nullptr);

	struct itimerspec its{
		.it_interval = {
			.tv_sec = 0,
			.tv_nsec = 300'000'000,
		},
		.it_value = {
			.tv_sec = 0,
			.tv_nsec = 400'000'000,
		},
	};

	ret = timer_settime(timer, 0, &its, nullptr);
	assert(ret == 0);

	pthread_mutex_lock(&mutex);

	while(count < COUNTER_EXPIRATIONS)
		pthread_cond_wait(&cond, &mutex);

	pthread_mutex_unlock(&mutex);

	struct timeval after;
	gettimeofday(&after, nullptr);

	struct timeval diff = {};
	timersub(&after, &before, &diff);
	assert(diff.tv_sec);
	fprintf(stderr, "diff %ld.%06ld (> 1)\n", diff.tv_sec, diff.tv_usec);

	ret = timer_delete(timer);
	assert(ret == 0);
}))
