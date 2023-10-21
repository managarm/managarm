#include <core/id-allocator.hpp>
#include <netserver/nic.hpp>

#include <algorithm>
#include <cstring>

#include <arch/bit.hpp>
#include <core/nic/buffer.hpp>
#include <helix/ipc.hpp>
#include <bragi/helpers-std.hpp>
#include <frg/formatting.hpp>
#include <frg/logging.hpp>

#include "ip/ip4.hpp"
#include "ip/arp.hpp"

#include "net.bragi.hpp"

namespace {

id_allocator<int> _allocator;

} /* namespace */

namespace nic {
uint8_t &MacAddress::operator[](size_t idx) {
	return mac_[idx];
}

const uint8_t &MacAddress::operator[](size_t idx) const {
	return mac_[idx];
}

bool operator==(const MacAddress &l, const MacAddress &r) {
	return std::equal(std::begin(l.mac_), std::end(l.mac_),
		std::begin(r.mac_));
}

bool operator!=(const MacAddress &l, const MacAddress &r) {
	return !operator==(l, r);
}

Link::Link(helix::UniqueLane lane, unsigned int mtu)
: index_{_allocator.allocate()}, _mtu(mtu), _lane{std::move(lane)} {

}

MacAddress& Link::deviceMac() {
	return mac_;
}

int Link::index() {
	return index_;
}

std::string Link::name() {
	std::string res;

	if(!mac_) {
		frg::output_to(res) << frg::fmt("eth{}", index_ - 1);
	} else {
		frg::output_to(res) << frg::fmt("enx{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac_[0], mac_[1], mac_[2], mac_[3], mac_[4], mac_[5]);
	}

	return res;
}

Link::AllocatedBuffer Link::allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize) {
	// default implementation assume an Ethernet II frame
	using namespace arch;
	Link::AllocatedBuffer buf {
		nic_core::buffer_view{ std::make_shared<nic_core::buffer_owner>(14 + payloadSize) }, {}
	};

	uint16_t et = static_cast<uint16_t>(type);
	et = convert_endian<endian::big>(et);
	std::memcpy(buf.frame.data(), to.data(), sizeof(MacAddress));
	std::memcpy(buf.frame.subview(6).data(), deviceMac().data(),
		sizeof(MacAddress));
	std::memcpy(buf.frame.subview(12).data(), &et, sizeof(et));

	buf.payload = buf.frame.subview(14);
	return buf;
}

async::result<void> Link::send(const nic_core::buffer_view packet) {
	managarm::net::Packet req;
	req.set_size(packet.size());

	auto [offer, sendReq, sendPacket, recvResp] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::sendBuffer(packet.data(), packet.size()),
				helix_ng::recvInline()
			)
		);

	// TODO: error handling
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(sendPacket.error());
	HEL_CHECK(recvResp.error());

	co_return;
}

async::result<bool> Link::updateMtu(unsigned int mtu, bool ask) {
	if(ask) {
		managarm::net::UpdateMTU req;
		req.set_mtu(mtu);

		auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);

		// TODO: error handling
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		auto resp = bragi::parse_head_only<managarm::net::SvrResponse>(recvResp);
		if(!resp) {
			std::cout << "netserver: updateMtu fails due to response decoding error!" << std::endl;
			co_return false;
		}

		if(resp->error() != managarm::net::Errors::SUCCESS)
			co_return false;
	}

	_mtu = mtu;

	co_return true;
}

async::detached Link::runDevice(helix::UniqueLane lane, std::shared_ptr<nic::Link> dev) {
	using namespace arch;

	co_await registerNic(dev);

	while(true) {
		auto [accept, recv_head] =
			co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
					)
				);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();
		auto sendError = [&conversation] (managarm::net::Errors err)
				-> async::result<void> {
			managarm::net::SvrResponse resp;
			resp.set_error(err);
			auto buff = resp.SerializeAsString();
			auto [send] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(
						buff.data(), buff.size())
				);
			HEL_CHECK(send.error());
		};

		auto sendSuccess = [&conversation] ()
				-> async::result<void> {
			managarm::net::SvrResponse resp;
			resp.set_error(managarm::net::Errors::SUCCESS);
			auto buff = resp.SerializeAsString();
			auto [send] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(
						buff.data(), buff.size())
				);
			HEL_CHECK(send.error());
		};

		auto preamble = bragi::read_preamble(recv_head);

		if(preamble.id() == managarm::net::Packet::message_id) {
			auto req = bragi::parse_head_only<managarm::net::Packet>(recv_head);
			if(!req) {
				std::cout << "netserver: Rejecting request due to decoding failure" << std::endl;
				continue;
			}
			
			if(req->size() > dev->mtu() + 14) {
				std::cout << "netserver: got too large packet from " << dev->name() << std::endl;
				sendError(managarm::net::Errors::PACKET_TOO_LARGE);
				continue;
			}

			// TODO: something faster
			//   also, we no longer need a dma_buffer
			// dma_buffer frameBuffer { dev->dmaPool(), req->size() };

			auto frameBuffer = nic_core::buffer_view{std::make_shared<nic_core::buffer_owner>(req->size())};

			auto [recv_packet] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(frameBuffer.data(), frameBuffer.size())
				);
			HEL_CHECK(recv_packet.error());

			auto capsule = frameBuffer.subview(14);
			auto data = reinterpret_cast<uint8_t*>(frameBuffer.data());
			uint16_t ethertype = data[12] << 8 | data[13];
			nic::MacAddress dstsrc[2];
			std::memcpy(dstsrc, data, sizeof(dstsrc));

			switch (ethertype) {
			case ETHER_TYPE_IP4:
				ip4().feedPacket(dstsrc[0], dstsrc[1],
					std::move(frameBuffer), capsule);
				break;
			case ETHER_TYPE_ARP:
				neigh4().feedArp(dstsrc[0], capsule, dev);
				break;
			default:
				break;
			}

			co_await sendSuccess();
		} else if(preamble.id() == managarm::net::UpdateMTU::message_id) {
			auto req = bragi::parse_head_only<managarm::net::UpdateMTU>(recv_head);
			if(!req) {
				std::cout << "netserver: Rejecting request due to decoding failure" << std::endl;
				continue;
			}

			dev->updateMtu(req->mtu(), false);
			co_await sendSuccess();
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

	co_await unregisterNic(dev);

	co_return;
}
} // namespace nic
