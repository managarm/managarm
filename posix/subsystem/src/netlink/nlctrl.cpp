#include <format>
#include <linux/genetlink.h>

#include "core/netlink.hpp"
#include "nlctrl.hpp"

namespace netlink {

struct GenericNetlinkOps {
	uint8_t cmd;
	uint8_t flags;
};

struct GenericNetlinkMulticastGroup {
	std::string name;
	uint8_t flags;
};

struct GenericNetlinkFamily {
	std::string name;
	uint32_t version;
	std::vector<GenericNetlinkOps> ops;
	std::vector<GenericNetlinkMulticastGroup> mcast_groups;
	int first_id;
};

std::vector<GenericNetlinkOps> genl_ctrl_ops = {
	{ CTRL_CMD_GETFAMILY, GENL_CMD_CAP_DO | GENL_CMD_CAP_DUMP },
	{ CTRL_CMD_GETPOLICY, GENL_CMD_CAP_DUMP },
};

std::vector<GenericNetlinkMulticastGroup> genl_ctrl_mcast_groups = {
	{ "notify", 0 },
};

std::map<uint16_t, GenericNetlinkFamily> families = {
	{GENL_ID_CTRL, { "nlctrl", 2, genl_ctrl_ops, genl_ctrl_mcast_groups, 0x10 }},
};

async::result<protocols::fs::Error> nlctrl::sendMsg(nl_socket::OpenFile *f, core::netlink::Packet packet, struct sockaddr_nl *sa) {
	auto nlh = reinterpret_cast<struct nlmsghdr *>(packet.buffer.data());
	auto genlh = reinterpret_cast<struct genlmsghdr *>(NLMSG_DATA(nlh));

	if(!sa->nl_pid)
		sa->nl_pid = f->socketPort();

	switch(nlh->nlmsg_type) {
		case GENL_ID_CTRL: {
			switch(genlh->cmd) {
				case CTRL_CMD_GETFAMILY: {
					auto req = core::netlink::netlinkAttr(nlh, core::netlink::nl::packets::genl{});

					std::optional<uint16_t> filter_id;
					std::optional<std::string> filter_name;

					if(req.has_value()) {
						for(auto val : *req) {
							switch(val.type()) {
								case CTRL_ATTR_FAMILY_ID:
									filter_id = val.data<uint16_t>();
									break;
								case CTRL_ATTR_FAMILY_NAME:
									filter_name = val.str();
									break;
								default:
									std::cout << std::format("posix: unhandled CTRL_CMD_GETFAMILY attr 0x{:x}\n", val.type());
									break;
							}
						}
					}

					size_t matches = 0;
					core::netlink::NetlinkBuilder b;

					for(auto const &[id, info] : families) {
						if(filter_id && filter_id.value() != id)
							continue;
						else if(filter_name && filter_name.value() != info.name)
							continue;

						b.header(id, NLM_F_MULTI, nlh->nlmsg_seq, sa->nl_pid);
						b.message<genlmsghdr>({
							.cmd = CTRL_CMD_NEWFAMILY,
							.version = 2,
						});
						b.nlattr<uint16_t>(CTRL_ATTR_FAMILY_ID, id);
						b.nlattr<std::string>(CTRL_ATTR_FAMILY_NAME, info.name);
						b.nlattr<uint32_t>(CTRL_ATTR_VERSION, info.version);
						b.nlattr<uint32_t>(CTRL_ATTR_HDRSIZE, 0);
						b.nlattr<uint32_t>(CTRL_ATTR_MAXATTR, 0);

						if(!info.ops.empty()) {
							b.nested_nlattr<const GenericNetlinkFamily *>(CTRL_ATTR_OPS,
									[](core::netlink::NetlinkBuilder &b, auto info) {
								for(size_t i = 0; i < info->ops.size(); i++) {
									struct OpInfo {
										uint8_t id;
										uint8_t flags;
									} const op_info = {
										info->ops[i].cmd,
										info->ops[i].flags,
									};

									b.nested_nlattr<const OpInfo *>(i + 1,
											[](core::netlink::NetlinkBuilder &b, auto ctx) {
										b.nlattr<uint32_t>(CTRL_ATTR_OP_ID, ctx->id);
										b.nlattr<uint32_t>(CTRL_ATTR_OP_FLAGS, ctx->flags);
									}, &op_info);
								}
							}, &info);
						}

						if(!info.mcast_groups.empty()) {
							b.nested_nlattr<const GenericNetlinkFamily *>(CTRL_ATTR_MCAST_GROUPS,
									[](core::netlink::NetlinkBuilder &b, auto info) {
								for(uint32_t i = 0; i < info->mcast_groups.size(); i++) {
									struct MulticastGroupInfo {
										uint32_t id;
										std::string name;
									} const mcast_info = {
										info->first_id + i,
										info->mcast_groups[i].name,
									};

									b.nested_nlattr<const MulticastGroupInfo *>(i + 1,
											[](core::netlink::NetlinkBuilder &b, auto ctx) {
										b.nlattr<uint32_t>(CTRL_ATTR_MCAST_GRP_ID, ctx->id);
										b.nlattr<std::string>(CTRL_ATTR_MCAST_GRP_NAME, ctx->name);
									}, &mcast_info);
								}
							}, &info);
						}

						f->deliver(b.packet());
						matches++;
					}

					if(matches) {
						sendDone(f, nlh, sa);
					} else {
						sendError(f, nlh, ENOENT, sa);
					}

					co_return protocols::fs::Error::none;
				}
				default: {
					std::cout << std::format("posix: unknown nlctrl cmd=0x{:x} version={}\n", genlh->cmd, genlh->version);
					co_return protocols::fs::Error::illegalArguments;
				}
			}

			break;
		}
	}

	co_return protocols::fs::Error::illegalArguments;
}

} // namespace netlink
