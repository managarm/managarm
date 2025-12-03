#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

static pthread_rwlock_t rwlock;

static void *timed_rdlock_thread(void *) {
	struct timespec timeout;
	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += 1; // 1 second timeout

	int ret = pthread_rwlock_timedrdlock(&rwlock, &timeout);
	return (void *)(intptr_t)ret;
}

DEFINE_TEST(pthread_rwlock_timedrdlock_timeout, ([] {
	int ret = pthread_rwlock_init(&rwlock, NULL);
	assert(ret == 0);

	// Acquire a write lock to block readers
	ret = pthread_rwlock_wrlock(&rwlock);
	assert(ret == 0);

	pthread_t thread;
	ret = pthread_create(&thread, NULL, timed_rdlock_thread, NULL);
	assert(ret == 0);

	void *thread_ret;
	ret = pthread_join(thread, &thread_ret);
	assert(ret == 0);

	// The timedrdlock should have timed out
	assert((int)(intptr_t)thread_ret == ETIMEDOUT);

	// Release the write lock
	ret = pthread_rwlock_unlock(&rwlock);
	assert(ret == 0);

	ret = pthread_rwlock_destroy(&rwlock);
	assert(ret == 0);
}));

static void *timed_wrlock_thread(void *) {
	struct timespec timeout;
	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += 1; // 1 second timeout

	int ret = pthread_rwlock_timedwrlock(&rwlock, &timeout);
	return (void *)(intptr_t)ret;
}

DEFINE_TEST(pthread_rwlock_timedwrlock_timeout, ([] {
	int ret = pthread_rwlock_init(&rwlock, NULL);
	assert(ret == 0);

	// Acquire a read lock to block writers
	ret = pthread_rwlock_rdlock(&rwlock);
	assert(ret == 0);

	pthread_t thread;
	ret = pthread_create(&thread, NULL, timed_wrlock_thread, NULL);
	assert(ret == 0);

	void *thread_ret;
	ret = pthread_join(thread, &thread_ret);
	assert(ret == 0);

	// The timedwrlock should have timed out
	assert((int)(intptr_t)thread_ret == ETIMEDOUT);

	// Release the read lock
	ret = pthread_rwlock_unlock(&rwlock);
	assert(ret == 0);

	ret = pthread_rwlock_destroy(&rwlock);
	assert(ret == 0);
}));
