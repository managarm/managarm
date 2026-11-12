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
