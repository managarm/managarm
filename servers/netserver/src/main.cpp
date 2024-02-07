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

#include "ip/ip4.hpp"
#include "netlink/netlink.hpp"

#include <netserver/nic.hpp>
#include <nic/virtio/virtio.hpp>

// Maps mbus IDs to device objects
std::unordered_map<int64_t, std::shared_ptr<nic::Link>> baseDeviceMap;

std::optional<helix::UniqueDescriptor> posixLane;

std::unordered_map<int64_t, std::shared_ptr<nic::Link>> &nic::Link::getLinks() {
	return baseDeviceMap;
}

std::shared_ptr<nic::Link> nic::Link::byIndex(int index) {
	if(baseDeviceMap.empty())
		return {};

	for(auto it = baseDeviceMap.begin(); it != baseDeviceMap.end(); it++)
		if(it->second->index() == index)
			return it->second;

	return {};
}

async::result<void> doBind(mbus_ng::Entity base_entity, virtio_core::DiscoverMode discover_mode) {
	protocols::hw::Device hwDevice((co_await base_entity.getRemoteLane()).unwrap());
	co_await hwDevice.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hwDevice), discover_mode);

	auto device = nic::virtio::makeShared(std::move(transport));
	if (baseDeviceMap.empty()) {
		// default via 10.0.2.2 src 10.10.2.15
		Ip4Router::Route wan { { 0, 0 }, device };
		wan.gateway = 0x0a000202;
		wan.source = 0x0a0a020f;
		ip4Router().addRoute(std::move(wan));

		// 10.0.2.0/24
		ip4Router().addRoute({ { 0x0a000200, 24 }, device });
		// inet 10.10.2.15/24
		ip4().setLink({ 0x0a0a020f, 24 }, device);
	}
	baseDeviceMap.insert({base_entity.id(), device});
	nic::runDevice(device);
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "netserver: Binding to device " << base_id << std::endl;
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(baseDeviceMap.find(baseEntity.id()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = (co_await baseEntity.getProperties()).unwrap();
	if(auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
			!vendor_str || vendor_str->value != "1af4")
		co_return protocols::svrctl::Error::deviceNotSupported;

	virtio_core::DiscoverMode discover_mode;
	if(auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]); device_str) {
		if(device_str->value == "1000")
			discover_mode = virtio_core::DiscoverMode::transitional;
		else if(device_str->value == "1041")
			discover_mode = virtio_core::DiscoverMode::modernOnly;
		else
			co_return protocols::svrctl::Error::deviceNotSupported;
	}else{
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	co_await doBind(std::move(baseEntity), discover_mode);
	co_return protocols::svrctl::Error::success;
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

			posixLane = std::move(conversation);
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
}

async::detached advertise() {
	mbus_ng::Properties descriptor {
		{"class", mbus_ng::StringItem{"netserver"}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
		"netserver", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serve(std::move(localLane));
		}
	}(std::move(entity));
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("netserver: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	async::detach(protocols::svrctl::serveControl(&controlOps));
	advertise();
	async::run_forever(helix::currentDispatcher);
}
