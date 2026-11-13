#include <cassert>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(stat_pipe, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);

	struct stat res;
	e = fstat(fds[0], &res);
	assert(!e);
	assert(S_ISFIFO(res.st_mode));
	e = fstat(fds[1], &res);
	assert(!e);
	assert(S_ISFIFO(res.st_mode));

	close(fds[0]);
	close(fds[1]);
}))

DEFINE_TEST(stat_initrd_regular_file, ([] {
#if defined(__linux__)
	skip_test("requires the Managarm initrd");
#else
	// posix-init is loaded from the initrd during posix-subsystem startup.
	struct stat res{};
	assert(stat("/usr/bin/posix-init", &res) == 0);
	assert(S_ISREG(res.st_mode));
	assert(res.st_size > 0);
#endif
}))
