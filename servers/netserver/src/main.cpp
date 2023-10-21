#include <assert.h>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <protocols/fs/server.hpp>
#include <sys/socket.h>
#include "fs.bragi.hpp"
#include "net.bragi.hpp"

#include "ip/ip4.hpp"
#include "netlink/netlink.hpp"

#include <netserver/nic.hpp>


std::optional<helix::UniqueDescriptor> posixLane;

std::vector<std::shared_ptr<nic::Link>> registeredNics;

std::vector<std::shared_ptr<nic::Link>> &nic::getLinks() {
	return registeredNics;
}

std::shared_ptr<nic::Link> nic::byIndex(int index) {
	if(registeredNics.empty())
		return {};

	for(auto it = registeredNics.begin(); it != registeredNics.end(); it++)
		if((*it)->index() == index)
			return *it;

	return {};
}

async::result<void> nic::registerNic(std::shared_ptr<nic::Link> nic) {
	std::cout << "netserver: registering nic " << nic->name() << std::endl;

	if (registeredNics.empty()) {
		// default via 10.0.2.2 src 10.10.2.15
		Ip4Router::Route wan { { 0, 0 }, nic };
		wan.gateway = 0x0a000202;
		wan.source = 0x0a0a020f;
		ip4Router().addRoute(std::move(wan));

		// 10.0.2.0/24
		ip4Router().addRoute({ { 0x0a000200, 24 }, nic });
		// inet 10.10.2.15/24
		ip4().setLink({ 0x0a0a020f, 24 }, nic);
	}

	registeredNics.push_back(nic);
	co_return;
}

async::result<void> nic::unregisterNic(std::shared_ptr<nic::Link> nic) {
	std::cout << "netserver: unregistering nic " << nic->name() << std::endl;

	for(auto it = registeredNics.begin(); it != registeredNics.end(); it++) {
		if((*it) == nic){
			registeredNics.erase(it);
			co_return;
		}
	}

	co_return;
}

async::detached serve(helix::UniqueLane lane) {
	while (true) {
		auto [accept, recv_req] =
			co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
					)
				);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		auto sendError = [&conversation] (managarm::fs::Errors err)
				-> async::result<void> {
			managarm::fs::SvrResponse resp;
			resp.set_error(err);
			auto buff = resp.SerializeAsString();
			auto [send] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(
						buff.data(), buff.size())
				);
			HEL_CHECK(send.error());
		};

		auto preamble = bragi::read_preamble(recv_req);

		if(preamble.id() == managarm::fs::CntRequest::message_id) {
			managarm::fs::CntRequest req;
			req.ParseFromArray(recv_req.data(), recv_req.length());
			recv_req.reset();

			if (req.req_type() == managarm::fs::CntReqType::CREATE_SOCKET) {
				auto [local_lane, remote_lane] = helix::createStream();

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);

				if(req.domain() == AF_INET) {
					auto err = ip4().serveSocket(std::move(local_lane),
							req.type(), req.protocol(), req.flags());
					if(err != managarm::fs::Errors::SUCCESS) {
						co_await sendError(err);
						continue;
					}
				} else if(req.domain() == AF_NETLINK) {
					auto nl_socket = smarter::make_shared<nl::NetlinkSocket>(req.flags());
					async::detach(servePassthrough(std::move(local_lane), nl_socket,
							&nl::NetlinkSocket::ops));
				} else {
					std::cout << "mlibc: unexpected socket domain " << req.domain() << std::endl;
					co_await sendError(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					continue;
				}

				auto ser = resp.SerializeAsString();
				auto [send_resp, push_socket] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBuffer(
							ser.data(), ser.size()),
						helix_ng::pushDescriptor(remote_lane)
					);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_socket.error());
			} else {
				std::cout << "netserver: received unknown request type: "
					<< (int32_t)req.req_type() << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::dismiss()
				);
				HEL_CHECK(dismiss.error());
			}
		} else if(preamble.id() == managarm::fs::InitializePosixLane::message_id) {
			co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::dismiss()
			);

			*posixLane = std::move(conversation);

		// net protocol
		} else if(preamble.id() == managarm::net::RegisterNicRequest::message_id) {
			managarm::net::RegisterNicRequest req;
			auto [recv_lane] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::pullDescriptor()
				);

			HEL_CHECK(recv_lane.error());

			// TODO: use the type parameter
			req.ParseFromArray(recv_req.data(), recv_req.length());
			recv_req.reset();

			managarm::net::SvrResponse resp;

			auto nic_dev = std::make_shared<nic::Link>(recv_lane.descriptor(), req.mtu());
			memcpy(nic_dev->deviceMac().data(), req.mac().data(), 6);

			// TODO: verify permissions
			//   While creating NICs should be safe from a security perspective, it is probably
			//   not the best idea to allow anything and everything to make a new NIC
			
			// Create our lane, we use this to send packets to the nic driver
			auto [local_lane, remote_lane] = helix::createStream();

			nic::Link::runDevice(std::move(local_lane), std::move(nic_dev));

			// Push NIC comms lane
			auto ser = resp.SerializeAsString();
			auto [send_resp, push_send_lane] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_send_lane.error());
		} else {
			std::cout << "netserver: received unknown message: "
				<< preamble.id() << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::dismiss()
			);
			HEL_CHECK(dismiss.error());
		}
	}

	co_return;
}

async::detached advertise() {
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor {
		{"class", mbus::StringItem{"netserver"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		auto [local_lane, remote_lane] = helix::createStream();

		serve(std::move(local_lane));
		co_return std::move(remote_lane);
	});

	co_await root.createObject("netserver", descriptor, std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("netserver: Starting server\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	advertise();
	async::run_forever(helix::currentDispatcher);
}
