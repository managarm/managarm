#include <cassert>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "testsuite.hpp"

#define BOGUS_FD 1947830128

// TODO: openat is not implemented
/*DEFINE_TEST(openat_bad_dirfd, ([] {
	int fd = openat(BOGUS_FD, "foo", O_RDONLY);
	assert(fd == -1);
	assert(errno == EBADF);
}))*/

DEFINE_TEST(close_badfd, ([] {
	close(BOGUS_FD);
}))

DEFINE_TEST(dup_badfd, ([] {
	int fd = dup(BOGUS_FD);
	assert(fd == -1);
	assert(errno == EBADF);
}))

DEFINE_TEST(io_badfd, ([] {
	char buf[16];

	int bytes = read(BOGUS_FD, buf, 16);
	assert(bytes == -1);
	assert(errno == EBADF);
	bytes = write(BOGUS_FD, buf, 16);
	assert(bytes == -1);
	assert(errno == EBADF);
}))

DEFINE_TEST(stat_badfd, ([] {
	struct stat st;

	int ret = fstat(BOGUS_FD, &st);
	assert(ret == -1);
	assert(errno == EBADF);
}))
