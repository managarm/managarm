#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(procfs_status_after_wait, ([] {
	pid_t pid = fork();
	assert(pid >= 0);

	if(!pid) {
		_exit(0);
	}

	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);

	int fd = open(path, O_RDONLY);
	assert(fd >= 0);

	siginfo_t dummy;
	int ret = waitid(P_PID, pid, &dummy, WEXITED | WNOWAIT);
	assert(ret == 0);

	char buf[128];
	ret = read(fd, buf, sizeof(buf));
	assert(ret == 128);
	// rewind to invalidate caching
	lseek(fd, 0, SEEK_SET);

	int status;
	ret = waitpid(pid, &status, 0);
	assert(ret == pid);

	ret = read(fd, buf, sizeof(buf));
	assert(ret == -1);
	assert(errno == ESRCH);

	close(fd);
}))
