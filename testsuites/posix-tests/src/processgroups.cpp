#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "testsuite.hpp"

namespace {

// These tests need /dev/ptmx, absent in the build container. Probe in the parent
// (skip_test must not run in the forked child) and skip if it is unavailable.
void require_ptmx() {
	int fd = open("/dev/ptmx", O_RDWR);
	if(fd == -1)
		skip_test("/dev/ptmx unavailable");
	close(fd);
}

} // namespace

/*
 * The general gist of these tests is as follows:
 * We fork to get a clean slate,
 * We open /dev/ptmx to get a terminal,
 * We spawn a new session with setsid,
 * We make that opened terminal the controlling terminal,
 * Now we can run the required tests,
 * When done, exit the fork.
 * Yes, this is tedious, but the only way to guarantee a proper environment.
 */
DEFINE_TEST(tcgetsid, ([] {
	require_ptmx();
	pid_t pid = fork();
	if(!pid) {
		// Open the terminal
		int fd = open("/dev/ptmx", 0);
		assert(fd != -1);
		// Spawn a new session
		pid_t sid = setsid();
		assert(sid != -1); // Error return
		int mysid = getsid(getpid()); // Sanity checking
		assert(sid == mysid); // Are we really in a new session?
		// Make the terminal the controlling terminal
		int ret = ioctl(fd, TIOCSCTTY, 0);
		assert(ret == 0);
		int terminalsid = tcgetsid(fd); // Should be the same as sid and mysid now
		assert(mysid == terminalsid);
		exit(0);
	} else {
		waitpid(pid, NULL, 0);
	}
}))

DEFINE_TEST(tcgetpgrp, ([] {
	require_ptmx();
	pid_t pid = fork();
	if(!pid) {
		// Open the terminal
		int fd = open("/dev/ptmx", 0);
		assert(fd != -1);
		// Spawn a new session
		pid_t sid = setsid();
		assert(sid != -1); // Error return
		// Make the terminal the controlling terminal
		int ret = ioctl(fd, TIOCSCTTY, 0);
		assert(ret == 0);
		int pgrp_getpgid = getpgid(0);
		int pgrp_getpgrp = getpgrp();
		int pgrp_tcgetpgrp = tcgetpgrp(fd);
		// The three values above should all be the same
		assert(pgrp_getpgid == pgrp_getpgrp);
		assert(pgrp_getpgid == pgrp_tcgetpgrp);
		exit(0);
	} else {
		waitpid(pid, NULL, 0);
	}
}))

DEFINE_TEST(setsid, ([] {
	pid_t pid = fork();
	if(!pid) {
		int sid = getsid(getpid());
		int newsid = setsid();
		assert(newsid != -1); // -1 is the error return
		assert(newsid != sid); // We should be in a new session now
		newsid = setsid();
		assert(newsid == -1); // As the session leader, we can't spawn another session
		exit(0);
	} else {
		waitpid(pid, NULL, 0);
	}
}))

DEFINE_TEST(tcsetpgrp, ([] {
	require_ptmx();
	pid_t pid = fork();
	if(!pid) {
		// Open the terminal
		int fd = open("/dev/ptmx", 0);
		assert(fd != -1);
		// Spawn a new session
		pid_t sid = setsid();
		assert(sid != -1); // Error return
		// Make the terminal the controlling terminal
		int ret = ioctl(fd, TIOCSCTTY, 0);
		assert(ret == 0);
		int pgid = getpgid(0);
		ret = tcsetpgrp(fd, pgid);
		assert(ret == 0); // Checking for success
		ret = tcsetpgrp(fd, -1); // Invalid pgid
		assert(ret == -1); // Checking for error
		assert(errno == EINVAL);
		exit(0);
	} else {
		waitpid(pid, NULL, 0);
	}
}))

DEFINE_TEST(nonorphaned_background_process_group, ([] {
	            require_ptmx();
	            pid_t session = fork();
	            assert(session >= 0);
	            if (!session) {
		            alarm(5);
		            assert(setsid() >= 0);
		            int terminal = open("/dev/ptmx", O_RDWR | O_NOCTTY);
		            assert(terminal >= 0);
		            assert(!ioctl(terminal, TIOCSCTTY, 0));
		            pid_t child = fork();
		            assert(child >= 0);
		            if (!child) {
			            assert(!setpgid(0, 0));
			            struct sigaction action{};
			            action.sa_handler = [](int) {};
			            assert(!sigemptyset(&action.sa_mask));
			            assert(!sigaction(SIGTTOU, &action, nullptr));
			            // Our parent is in another group in the same session. A caught
			            // SIGTTOU can interrupt this call, but an orphan-group EIO is wrong.
			            auto result = tcsetpgrp(terminal, getpgrp());
			            assert(result == 0 || (result == -1 && errno == EINTR));
			            _exit(0);
		            }
		            int status;
		            assert(waitpid(child, &status, 0) == child);
		            assert(WIFEXITED(status) && !WEXITSTATUS(status));
		            assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
		            close(terminal);
		            _exit(0);
	            }
	            int status;
	            assert(waitpid(session, &status, 0) == session);
	            assert(WIFEXITED(status) && !WEXITSTATUS(status));
            }))
