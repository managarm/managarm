#include <cassert>
#include <cstring>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(epoll_mod_active, ([] {
	            int e;
	            int pending;

	            int fd = eventfd(0, 0);
	            assert(fd >= 0);

	            int epfd = epoll_create1(0);
	            assert(epfd >= 0);

	            epoll_event evt;

	            memset(&evt, 0, sizeof(epoll_event));
	            evt.events = 0;
	            e = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
	            assert(!e);

	            // Nothing should be pending.
	            memset(&evt, 0, sizeof(epoll_event));
	            pending = epoll_wait(epfd, &evt, 1, 0);
	            assert(!pending);

	            uint64_t n = 1;
	            auto written = write(fd, &n, sizeof(uint64_t));
	            assert(written == sizeof(uint64_t));

	            memset(&evt, 0, sizeof(epoll_event));
	            evt.events = EPOLLIN;
	            e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	            assert(!e);

	            // The FD should be pending now.
	            memset(&evt, 0, sizeof(epoll_event));
	            pending = epoll_wait(epfd, &evt, 1, 0);
	            assert(pending == 1);
	            assert(evt.events & EPOLLIN);

	            close(epfd);
	            close(fd);
            }))
