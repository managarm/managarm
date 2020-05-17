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

