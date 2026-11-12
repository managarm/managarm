#include "testsuite.hpp"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

// These regressions assert Managarm PTY behavior, including its output queue semantics.
#if defined(__managarm__)
namespace {

struct Pty {
	Pty() {
		assert(!openpty(&master, &slave, nullptr, nullptr, nullptr));
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
	            attrs.c_lflag |= IEXTEN;
	            for (int index : {VINTR, VQUIT, VSUSP, VEOF, VEOL, VEOL2, VKILL, VERASE, VWERASE})
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
	            alarm(5);
	            Pty pty;
	            auto flags = fcntl(pty.slave, F_GETFL);
	            assert(flags >= 0);
	            assert(!fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK));
	            auto current = fcntl(pty.slave, F_GETFL);
	            assert(current >= 0 && (current & O_NONBLOCK));
	            char byte;
	            assert(read(pty.slave, &byte, 1) == -1 && errno == EAGAIN);
	            assert(!fcntl(pty.slave, F_SETFL, flags & ~O_NONBLOCK));
	            assert(!(fcntl(pty.slave, F_GETFL) & O_NONBLOCK));
	            alarm(0);
            }))

DEFINE_TEST(pty_leave_canonical_mode, ([] {
	            Pty pty;
	            for (int control : {pty.slave, pty.master}) {
		            auto attrs = canonicalSettings(control);
		            assert(!tcsetattr(control, TCSANOW, &attrs));
		            assert(write(pty.master, "partial", 7) == 7);
		            assert(!readable(pty.slave));
		            attrs.c_lflag &= ~ICANON;
		            assert(!tcsetattr(control, TCSANOW, &attrs));
		            expectInput(pty.slave, "partial", 7);
	            }
	            // Moving input state into the channel must retain editing across writes.
	            auto attrs = canonicalSettings(pty.slave);
	            attrs.c_cc[VERASE] = '\b';
	            assert(!tcsetattr(pty.slave, TCSANOW, &attrs));
	            assert(write(pty.master, "ab", 2) == 2);
	            assert(write(pty.master, "\bC\n", 3) == 3);
	            expectInput(pty.slave, "aC\n", 3);
            }))
#endif
