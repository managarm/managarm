#include <cassert>

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "testsuite.hpp"

DEFINE_TEST(memfd_create, ([] {
	int fd = memfd_create("posix-tests", 0);
	assert(fd != -1);

	int ret = ftruncate(fd, 0x1000);
	assert(ret == 0);

	ret = close(fd);
	assert(ret == 0);
}))
