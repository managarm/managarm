#include <cassert>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#include "testsuite.hpp"

DEFINE_TEST(pipe_close_writer, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);
	close(fds[1]); // Close writer.

	pollfd pfd;
	memset(&pfd, 0, sizeof(pollfd));
	pfd.fd = fds[0];
	e = poll(&pfd, 1, 0); // Non-blocking poll().
	assert(e == 1);
	assert(!(pfd.revents & POLLIN));
	assert(!(pfd.revents & POLLERR));
	assert(pfd.revents & POLLHUP);
}))

DEFINE_TEST(pipe_close_reader, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);
	close(fds[0]); // Close reader.

	pollfd pfd;
	memset(&pfd, 0, sizeof(pollfd));
	pfd.fd = fds[1];
	e = poll(&pfd, 1, 0); // Non-blocking poll().
	assert(e == 1);
	assert(!(pfd.revents & POLLOUT));
	assert(pfd.revents & POLLERR);
	assert(!(pfd.revents & POLLHUP));
}))

DEFINE_TEST(pipe_blocked_writer_reader_close, ([] {
	int fds[2];
	int synchronize[2];
	assert(pipe(fds) == 0);
	assert(pipe(synchronize) == 0);

	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(fds[1]);
		close(synchronize[1]);
		char byte;
		assert(read(synchronize[0], &byte, 1) == 1);
		close(fds[0]);
		_exit(0);
	}

	close(fds[0]);
	close(synchronize[0]);
	void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
	assert(old_sigpipe != SIG_ERR);

	assert(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
	int nonBlockFlags = fcntl(fds[1], F_GETFL, 0);
	assert(nonBlockFlags >= 0);
	assert(nonBlockFlags & O_NONBLOCK);

	// Fill the pipe without relying on its implementation-specific capacity.
	char buffer[4096] = {};
	while (true) {
		ssize_t ret = write(fds[1], buffer, sizeof(buffer));
		if (ret < 0) {
			assert(errno == EAGAIN || errno == EWOULDBLOCK);
			break;
		}
		assert(ret == sizeof(buffer));
	}

	assert(fcntl(fds[1], F_SETFL, 0) == 0);
	int blockingFlags = fcntl(fds[1], F_GETFL, 0);
	assert(blockingFlags >= 0);
	assert(!(blockingFlags & O_NONBLOCK));
	char byte = 0;
	assert(write(synchronize[1], &byte, 1) == 1);
	close(synchronize[1]);

	// The pipe is full, so this write blocks until the child closes its reader.
	ssize_t ret = write(fds[1], &byte, 1);
	assert(ret == -1);
	assert(errno == EPIPE);

	signal(SIGPIPE, old_sigpipe);
	close(fds[1]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}))

DEFINE_TEST(pipe_default_sigpipe, ([] {
	int fds[2];
	int synchronize[2];
	assert(pipe(fds) == 0);
	assert(pipe(synchronize) == 0);

	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
		close(fds[0]);
		close(synchronize[1]);
		char byte;
		assert(read(synchronize[0], &byte, 1) == 1);
		write(fds[1], &byte, 1);
		_exit(1);
	}

	close(fds[0]);
	close(fds[1]);
	close(synchronize[0]);
	char byte = 0;
	assert(write(synchronize[1], &byte, 1) == 1);
	close(synchronize[1]);

	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFSIGNALED(status));
	assert(WTERMSIG(status) == SIGPIPE);
}))

DEFINE_TEST(fifo_rw, ([] {
	assert(mkfifo("/tmp/posix-testsuite-fifo", S_IRUSR | S_IWUSR) == 0);

	int fd = open("/tmp/posix-testsuite-fifo", O_RDWR | O_NONBLOCK);
	assert(fd >= 0);

	char buf[1] = {42};
	assert(write(fd, buf, 1) > 0);
	assert(read(fd, buf, 1) > 0);

	assert(close(fd) == 0);
	assert(unlink("/tmp/posix-testsuite-fifo") == 0);
}))
