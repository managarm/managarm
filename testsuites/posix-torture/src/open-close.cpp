#include <cassert>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(open_close_netlink, ([] {
	            int fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
	            assert(fd > 0);
	            close(fd);
            }))

DEFINE_TEST(open_close_timerfd, ([] {
	            int fd = timerfd_create(CLOCK_MONOTONIC, 0);
	            assert(fd > 0);
	            close(fd);
            }))

DEFINE_TEST(open_close_unix, ([] {
	            int fd = socket(PF_UNIX, SOCK_DGRAM, 0);
	            assert(fd > 0);
	            close(fd);
            }))
