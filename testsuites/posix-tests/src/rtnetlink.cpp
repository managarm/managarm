#include <cassert>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "testsuite.hpp"

DEFINE_TEST(rtnetlink_getroute, ([] {
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	assert(fd);

	{
		struct nlmsghdr *msg;
		struct rtgenmsg *rtmsg;
		char buf[NLMSG_SPACE(sizeof(*rtmsg))];

		memset(buf, 0, sizeof(buf));
		msg = (struct nlmsghdr *) buf;
		msg->nlmsg_len = NLMSG_LENGTH (sizeof (*rtmsg));
		msg->nlmsg_type = RTM_GETROUTE;
		msg->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
		msg->nlmsg_pid = 0;

		rtmsg = (struct rtgenmsg *) NLMSG_DATA(msg);
		rtmsg->rtgen_family = AF_UNSPEC;

		assert(send(fd, buf, sizeof(buf), 0));
	}

	{
		struct nlmsghdr *msg;
		struct rtgenmsg *rtmsg;
		struct rtattr *rtattr;
		char buf[8192];


		ssize_t len = recv(fd, buf, sizeof(buf), 0);
		assert(len);

		msg = (struct nlmsghdr *) buf;
		assert(NLMSG_OK(msg, len));
		assert(msg->nlmsg_type == RTM_NEWROUTE);

		rtmsg = (struct rtgenmsg *) NLMSG_DATA(msg);
		int attrlen = NLMSG_PAYLOAD(msg, sizeof(struct rtmsg));
		assert(attrlen);

		rtattr = RTM_RTA(rtmsg);
		bool dst, src, gateway;
		dst = src = gateway = false;
		while (RTA_OK(rtattr, attrlen)) {
			if (rtattr->rta_type == RTA_DST)
				dst = true;
			else if (rtattr->rta_type == RTA_SRC)
				src = true;
			else if (rtattr->rta_type == RTA_GATEWAY)
				gateway = true;
			rtattr = RTA_NEXT(rtattr, attrlen);
		}

		assert(dst && src && gateway);
	}
}))
