#include <assert.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <unistd.h>
#include <sys/wait.h>

#include "testsuite.hpp"

DEFINE_TEST(eventfd, ([] {
	int fd = eventfd(0, 0);
	assert(fd != -1);
	uint64_t val;

	// Test basic read/write
	eventfd_write(fd, 5);
	eventfd_read(fd, &val);
	assert(val == 5);

	// Test that multiple writes are cumulative
	eventfd_write(fd, 2);
	eventfd_write(fd, 3);
	eventfd_read(fd, &val);
	assert(val == 5);

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	// Test that read clears the value
	int ret = eventfd_read(fd, &val);
	assert(ret == -1);
	assert(errno == EAGAIN);

	close(fd);
}))

DEFINE_TEST(eventfd_nonblock, ([] {
	int fd = eventfd(0, EFD_NONBLOCK);
	assert(fd != -1);
	uint64_t val;

	// Test that read on an empty eventfd returns EAGAIN
	ssize_t ret = read(fd, &val, sizeof(uint64_t));
	assert(ret == -1);
	assert(errno == EAGAIN);

	// Test write
	eventfd_write(fd, 2);
	eventfd_write(fd, 3);
	eventfd_read(fd, &val);
	assert(val == 5);

	close(fd);
}))

DEFINE_TEST(eventfd_semaphore, ([] {
	int fd = eventfd(0, EFD_SEMAPHORE);
	assert(fd != -1);
	uint64_t val;

	// Test that multiple writes are available as single units
	eventfd_write(fd, 3);
	eventfd_read(fd, &val);
	assert(val == 1);
	eventfd_read(fd, &val);
	assert(val == 1);
	eventfd_read(fd, &val);
	assert(val == 1);

	struct pollfd fds{
		.fd = fd,
		.events = POLLIN | POLLOUT,
		.revents = 0,
	};

	int ret = poll(&fds, 1, 0);
	assert(ret == 1);
	assert(fds.revents == POLLOUT);

	close(fd);
}))

DEFINE_TEST(eventfd_errors, ([] {
	// Test that eventfd fails with invalid flags
	int fd = eventfd(0, -1);
	assert(fd == -1);
	assert(errno == EINVAL);

	// Test that read with a small buffer fails
	fd = eventfd(0, 0);
	assert(fd != -1);
	uint32_t val;
	ssize_t ret = read(fd, &val, sizeof(uint32_t));
	assert(ret == -1);
	assert(errno == EINVAL);

	close(fd);
}))
