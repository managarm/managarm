#include <assert.h>
#include <net/if.h>
#include <netinet/in.h>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <async/result.hpp>
#include <bragi/helpers-std.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <protocols/fs/server.hpp>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include "fs.bragi.hpp"

#include "ip/ip4.hpp"
#include "netlink/netlink.hpp"
#include "raw.hpp"

#include <netserver/nic.hpp>
#include <nic/virtio/virtio.hpp>
#include <nic/usb_ecm/usb_ecm.hpp>
#include <protocols/usb/client.hpp>

// Maps mbus IDs to device objects
std::unordered_map<int64_t, std::shared_ptr<nic::Link>> baseDeviceMap;

std::optional<helix::UniqueDescriptor> posixLane;

std::unordered_map<int64_t, std::shared_ptr<nic::Link>> &nic::Link::getLinks() {
	return baseDeviceMap;
}

std::shared_ptr<nic::Link> nic::Link::byIndex(int index) {
	for(auto it = baseDeviceMap.begin(); it != baseDeviceMap.end(); it++)
		if(it->second->index() == index)
			return it->second;

	return {};
}

std::shared_ptr<nic::Link> nic::Link::byName(std::string name) {
	for(auto it = baseDeviceMap.begin(); it != baseDeviceMap.end(); it++)
		if(it->second->name() == name)
			return it->second;

	return {};
}

async::result<void> doBind(mbus_ng::Entity base_entity, virtio_core::DiscoverMode discover_mode) {
	protocols::hw::Device hwDevice((co_await base_entity.getRemoteLane()).unwrap());
	co_await hwDevice.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hwDevice), discover_mode);

	auto device = nic::virtio::makeShared(std::move(transport));

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
	auto type = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if(!type)
		co_return protocols::svrctl::Error::deviceNotSupported;

	if(type->value == "pci") {
		if(auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
				!vendor_str || vendor_str->value != "1af4")
			co_return protocols::svrctl::Error::deviceNotSupported;

		virtio_core::DiscoverMode discover_mode{};
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
	} else if(type->value == "usb") {
		auto dev = protocols::usb::connect((co_await baseEntity.getRemoteLane()).unwrap());
		std::optional<uint8_t> iMACAddress;
		std::optional<uint8_t> data_if;

		std::optional<int> in_endp_number;
		std::optional<int> out_endp_number;

		auto raw_descs = (co_await dev.configurationDescriptor(1)).value();
		protocols::usb::walkConfiguration(raw_descs, [&] (int type, size_t, void *descriptor, const auto &info) {
			if(type == protocols::usb::descriptor_type::cs_interface) {
				auto desc = reinterpret_cast<protocols::usb::CdcDescriptor *>(descriptor);

				switch(desc->subtype) {
					using CdcSubType = protocols::usb::CdcDescriptor::CdcSubType;

					case CdcSubType::Header: {
						auto hdr = reinterpret_cast<protocols::usb::CdcHeader *>(descriptor);
						printf("netserver: CDC version 0x%x\n", hdr->bcdCDC);
						break;
					}
					case CdcSubType::AbstractControl: {
						auto hdr = reinterpret_cast<protocols::usb::CdcAbstractControl *>(descriptor);
						printf("netserver: ACM capabilities 0x%02x\n", hdr->bmCapabilities);
						break;
					}
					case CdcSubType::Union: {
						auto hdr = reinterpret_cast<protocols::usb::CdcUnion *>(descriptor);
						assert(hdr->length >= 5);
						printf("netserver: controller interface %u\n", hdr->bControlInterface);
						for(size_t i = 0; i < hdr->length - offsetof(protocols::usb::CdcUnion, bSubordinateInterface); i++) {
							printf("netserver: sub interface #%zu: IF %u\n", i, hdr->bSubordinateInterface[i]);
						}

						data_if = hdr->bSubordinateInterface[0];
						break;
					}
					case CdcSubType::EthernetNetworking: {
						auto hdr = reinterpret_cast<protocols::usb::CdcEthernetNetworking *>(descriptor);
						printf("netserver: iMACAddress %u\n", hdr->iMACAddress);
						iMACAddress = hdr->iMACAddress;
						break;
					}
					default: {
						printf("netserver: unhandled Function Descriptor SubType %hhu\n", desc->subtype);
					}
				}
			} else if(type == protocols::usb::descriptor_type::endpoint) {
				if(info.interfaceNumber && *info.interfaceNumber == data_if && info.endpointType == protocols::usb::EndpointType::bulk) {
					if(info.endpointIn.value()) {
						in_endp_number = info.endpointNumber;
					} else {
						out_endp_number = info.endpointNumber;
					}
				}
			}
		});

		if(!iMACAddress || !data_if)
			co_return protocols::svrctl::Error::deviceNotSupported;

		auto str = (co_await dev.getString(*iMACAddress)).value();

		auto decodeHexString = [](char c) -> uint8_t {
			if(c >= 'A' && c <= 'F')
				return c - 'A' + 0x0A;
			if(c >= 'a' && c <= 'f')
				return c - 'a' + 0x0A;
			if(c >= '0' && c <= '9')
				return c - '0';
			__builtin_unreachable();
		};

		nic::MacAddress mac{{
			static_cast<uint8_t>((decodeHexString(str[0]) << 4) | decodeHexString(str[1])),
			static_cast<uint8_t>((decodeHexString(str[2]) << 4) | decodeHexString(str[3])),
			static_cast<uint8_t>((decodeHexString(str[4]) << 4) | decodeHexString(str[5])),
			static_cast<uint8_t>((decodeHexString(str[6]) << 4) | decodeHexString(str[7])),
			static_cast<uint8_t>((decodeHexString(str[8]) << 4) | decodeHexString(str[9])),
			static_cast<uint8_t>((decodeHexString(str[10]) << 4) | decodeHexString(str[11])),
		}};

		auto config = (co_await dev.useConfiguration(1)).unwrap();
		auto intf = (co_await config.useInterface(*data_if, 1)).unwrap();
		auto in = (co_await intf.getEndpoint(protocols::usb::PipeType::in, in_endp_number.value())).unwrap();
		auto out = (co_await intf.getEndpoint(protocols::usb::PipeType::out, out_endp_number.value())).unwrap();

		auto device = nic::usb_ecm::makeShared(std::move(dev), mac, std::move(in), std::move(out));

		baseDeviceMap.insert({baseEntity.id(), device});
		nic::runDevice(device);
	}

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
		assert(!preamble.error());

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
				} else if(req.domain() == AF_PACKET) {
					auto err = raw().serveSocket(std::move(local_lane),
							req.type(), req.protocol(), req.flags());
					if(err != managarm::fs::Errors::SUCCESS) {
						co_await sendError(err);
						continue;
					}
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
		}else if(preamble.id() == managarm::fs::IfreqRequest::message_id) {
			managarm::fs::IfreqRequest req;
			req.ParseFromArray(recv_req.data(), recv_req.length());
			recv_req.reset();

			managarm::fs::IfreqReply resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

			if(req.command() == SIOCGIFCONF) {
				std::vector<managarm::fs::Ifconf> ifconf;

				for(auto [_, link] : nic::Link::getLinks()) {
					auto addr_check = ip4().getCidrByIndex(link->index());
					if(!addr_check)
						continue;

					managarm::fs::Ifconf conf;
					conf.set_name(link->name());
					conf.set_ip4(addr_check->ip);
					ifconf.push_back(conf);
				}

				managarm::fs::IfconfReply resp;

				resp.set_ifconf(std::move(ifconf));
				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto [send_head, send_tail] =
					co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
					);
				HEL_CHECK(send_head.error());
				HEL_CHECK(send_tail.error());

				continue;
			}else if(req.command() == SIOCGIFNETMASK) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					auto addr_check = ip4().getCidrByIndex(link->index());

					if(addr_check) {
						resp.set_ip4_netmask(addr_check.value().mask());
						resp.set_error(managarm::fs::Errors::SUCCESS);
					}else {
						resp.set_ip4_netmask(0);
					}
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFINDEX) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					resp.set_index(link->index());
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFNAME) {
				auto link = nic::Link::byIndex(req.index());

				if(link) {
					resp.set_name(link->name());
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFFLAGS) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					resp.set_flags(IFF_UP | IFF_RUNNING | IFF_MULTICAST | IFF_BROADCAST);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFADDR) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					auto addr_check = ip4().getCidrByIndex(link->index());

					if(addr_check) {
						resp.set_ip4_addr(addr_check->ip);
						resp.set_error(managarm::fs::Errors::SUCCESS);
					} else {
						resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					}
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFMTU) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					resp.set_mtu(link->mtu);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFBRDADDR) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					auto addr_check = ip4().getCidrByIndex(link->index());

					if(addr_check) {
						auto addr = addr_check.value().ip;
						auto mask = addr_check.value().mask();
						auto wildcard_bits = ~mask;
						addr &= mask;
						addr |= wildcard_bits;

						resp.set_ip4_broadcast_addr(addr);
						resp.set_error(managarm::fs::Errors::SUCCESS);
					} else {
						resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					}
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}else if(req.command() == SIOCGIFHWADDR) {
				auto link = nic::Link::byName(req.name());

				if(link) {
					std::array<uint8_t, 6> mac{};
					memcpy(mac.data(), link->deviceMac().data(), 6);
					resp.set_mac(mac);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			}

			auto [send] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(send.error());
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
