#include "testsuite.hpp"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

struct Pty {
	Pty() {
		if (openpty(&master, &slave, nullptr, nullptr, nullptr)) {
			if (errno == ENOENT || errno == EACCES || errno == ENXIO)
				skip_test("pty devices unavailable");
			assert(!"openpty failed");
		}
	}
	~Pty() {
		if (master >= 0)
			close(master);
		if (slave >= 0)
			close(slave);
	}
	int master = -1;
	int slave = -1;
};

bool readable(int fd, int timeout = 0) {
	struct pollfd pfd{fd, POLLIN, 0};
	int result;
	do {
		result = poll(&pfd, 1, timeout);
	} while (result < 0 && errno == EINTR);
	assert(result >= 0);
	return result && (pfd.revents & POLLIN);
}

void expectInput(int fd, const char *expected, size_t length) {
	char buffer[64];
	assert(length <= sizeof(buffer));
	assert(readable(fd, 2000));
	assert(read(fd, buffer, length) == static_cast<ssize_t>(length));
	assert(!memcmp(buffer, expected, length));
}

struct termios canonicalSettings(int fd) {
	struct termios attrs;
	assert(!tcgetattr(fd, &attrs));
	attrs.c_iflag = 0;
	attrs.c_oflag = 0;
	attrs.c_lflag = ICANON | ISIG;
	attrs.c_cc[VMIN] = 1;
	attrs.c_cc[VTIME] = 0;
	return attrs;
}

} // namespace

DEFINE_TEST(pty_disabled_control_characters, ([] {
	            Pty pty;
	            auto attrs = canonicalSettings(pty.slave);
	            for (int index : {VINTR, VQUIT, VSUSP, VEOF, VEOL, VEOL2})
		            attrs.c_cc[index] = _POSIX_VDISABLE;
	            assert(!tcsetattr(pty.slave, TCSANOW, &attrs));
	            assert(write(pty.master, "\0", 1) == 1);
	            assert(!readable(pty.slave));
	            assert(write(pty.master, "\n", 1) == 1);
	            expectInput(pty.slave, "\0\n", 2);

	            attrs.c_cc[VINTR] = 0xff;
	            assert(!tcsetattr(pty.slave, TCSANOW, &attrs));
	            const char input[] = {static_cast<char>(0xff), 'x', '\n'};
	            assert(write(pty.master, input, sizeof(input)) == sizeof(input));
	            expectInput(pty.slave, "x\n", 2);
            }))

DEFINE_TEST(pty_slave_set_nonblocking, ([] {
	            Pty pty;
	            auto flags = fcntl(pty.slave, F_GETFL);
	            assert(flags >= 0);
	            assert(!fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK));
	            auto current = fcntl(pty.slave, F_GETFL);
	            assert(current >= 0 && (current & O_NONBLOCK));
	            assert((current & O_ACCMODE) == (flags & O_ACCMODE));
	            char byte;
	            errno = 0;
	            assert(read(pty.slave, &byte, 1) == -1 && errno == EAGAIN);
	            assert(!fcntl(pty.slave, F_SETFL, flags & ~O_NONBLOCK));
	            assert(!(fcntl(pty.slave, F_GETFL) & O_NONBLOCK));
	            assert(write(pty.master, "line\n", 5) == 5);
	            expectInput(pty.slave, "line\n", 5);
            }))

DEFINE_TEST(pty_blocked_read_hangup, ([] {
	            Pty pty;
	            int ready[2];
	            assert(!pipe(ready));
	            pid_t child = fork();
	            assert(child >= 0);
	            if (!child) {
		            alarm(5);
		            close(ready[0]);
		            close(pty.master);
		            pty.master = -1;
		            assert(write(ready[1], "r", 1) == 1);
		            char byte;
		            auto result = read(pty.slave, &byte, 1);
#if defined(__linux__)
		            // Linux can return EIO to a read already pending at hangup.
		            assert(result == 0 || (result == -1 && errno == EIO));
#else
		            assert(result == 0);
#endif
		            _exit(0);
	            }
	            close(ready[1]);
	            expectInput(ready[0], "r", 1);
	            close(ready[0]);
	            usleep(20000);
	            close(pty.master);
	            pty.master = -1;
	            int status;
	            assert(waitpid(child, &status, 0) == child);
	            assert(WIFEXITED(status) && !WEXITSTATUS(status));
	            char byte;
	            assert(read(pty.slave, &byte, 1) == 0);
            }))
