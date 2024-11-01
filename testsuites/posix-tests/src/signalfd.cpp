#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(signalfd_nonblock, ([] {
	            int e;

	            sigset_t sigSet;
	            sigemptyset(&sigSet);
	            sigaddset(&sigSet, SIGUSR1);

	            sigset_t oldSet;
	            e = sigprocmask(SIG_BLOCK, &sigSet, &oldSet);
	            assert(!e);

	            int fd = signalfd(-1, &sigSet, SFD_NONBLOCK);
	            assert(fd >= 0);

	            // Check if the signalfd is readable.
	            signalfd_siginfo si;
	            ssize_t sz;

	            sz = read(fd, &si, sizeof(signalfd_siginfo));
	            assert(sz == -1);
	            assert(errno = EAGAIN);

	            // Raise a signal.
	            e = kill(getpid(), SIGUSR1);
	            assert(!e);

	            // Check again.
	            sz = read(fd, &si, sizeof(signalfd_siginfo));
	            assert(sz == sizeof(signalfd_siginfo));
	            assert(si.ssi_signo == SIGUSR1);

	            e = sigprocmask(SIG_SETMASK, &oldSet, nullptr);
	            assert(!e);

	            close(fd);
            }))
