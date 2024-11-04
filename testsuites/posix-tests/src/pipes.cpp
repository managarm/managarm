#include <cassert>
#include <cstring>
#include <errno.h>
#include <poll.h>
#include <unistd.h>

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
