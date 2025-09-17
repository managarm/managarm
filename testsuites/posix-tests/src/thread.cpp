#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "testsuite.hpp"

struct join_test_data {
	pthread_t mainThread;
	int efd;
};

static void *thread_A_func(void *arg) {
	auto data = static_cast<join_test_data *>(arg);

	uint64_t val = 1;
	ssize_t bytes_written = write(data->efd, &val, sizeof(uint64_t));
	assert(bytes_written == sizeof(uint64_t));

	void *code;
	pthread_join(data->mainThread, &code);
	fprintf(stderr, "main thread exited with 0x%lx\n", (uintptr_t) code);
	assert((uintptr_t) code == 0xDEAD);

	exit(0);
}

DEFINE_TEST(pthread_join_on_exiting_thread, ([] {
	int efd = eventfd(0, 0);
	assert(efd >= 0);

	join_test_data data;
	data.efd = efd;
	data.mainThread = pthread_self();

	pthread_t thread_A;
	int ret = pthread_create(&thread_A, nullptr, thread_A_func, &data);
	assert(ret == 0);

	uint64_t val;
	ssize_t read_len = read(efd, &val, sizeof(val));
	fprintf(stderr, "read returns %ld\n", read_len);
	assert(read_len == 8);

	pthread_exit((void *) 0xDEAD);
}));
