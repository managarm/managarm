#include <algorithm>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <bragi/helpers-std.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <core/nic/core.hpp>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include "net.bragi.hpp"

namespace nic_core {

Nic::Nic(arch::dma_pool *dmaPool)
: mac{6}, dmaPool_{dmaPool} {

}

async::result<void> Nic::updateMtu(unsigned int new_mtu) {
	managarm::net::UpdateMTU req;
	req.set_mtu(new_mtu);

	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		to_netserver,
		helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	// TODO: error handling
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
}

async::detached Nic::doSendPackets() {
	while(true) {
		auto [accept, recv_head] =
			co_await helix_ng::exchangeMsgs(
				from_netserver,
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
				std::cout << "core/nic: Rejecting request due to decoding failure" << std::endl;
				continue;
			}

			if(req->size() > 1514) {
				std::cout << "core/nic: got too large packet!" << std::endl;
				sendError(managarm::net::Errors::PACKET_TOO_LARGE);
				continue;
			}

			arch::dma_buffer frameBuffer { dmaPool_, req->size() };

			auto [recv_packet] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(frameBuffer.data(), frameBuffer.size())
				);
			HEL_CHECK(recv_packet.error());

			co_await send(frameBuffer);
			co_await sendSuccess();
		} else if(preamble.id() == managarm::net::UpdateMTU::message_id) {
			auto req = bragi::parse_head_only<managarm::net::UpdateMTU>(recv_head);
			if(!req) {
				std::cout << "core/nic: Rejecting request due to decoding failure" << std::endl;
				continue;
			}

			if(!co_await verifyMtu(req->mtu())) {
				co_await sendError(managarm::net::Errors::UPDATE_NOT_ALLOWED);
				continue;
			}

			mtu_ = req->mtu();
			co_await sendSuccess();
		} else {
			std::cout << "core/nic: received unknown message: "
				<< preamble.id() << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::dismiss()
			);
			HEL_CHECK(dismiss.error());
		}
	}
}

async::detached Nic::doRecvPackets() {
	while(true) {
		arch::dma_buffer frameBuffer { dmaPool_, 1514};
		co_await receive(frameBuffer);

		managarm::net::Packet req;
		req.set_size(frameBuffer.size());

		auto [offer, sendReq, sendPacket, recvResp] = co_await helix_ng::exchangeMsgs(
			to_netserver,
			helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::sendBuffer(frameBuffer.data(), frameBuffer.size()),
					helix_ng::recvInline()
				)
			);

		// TODO: error handling
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendPacket.error());
		HEL_CHECK(recvResp.error());
	}
}

async::result<void> Nic::startDevice(helix::UniqueLane packet_recv_lane, helix::UniqueLane packet_send_lane) {
	to_netserver = std::move(packet_recv_lane);
	from_netserver = std::move(packet_send_lane);

	doSendPackets();
	doRecvPackets();
	co_return;
}

async::result<void> Nic::doBind(helix::UniqueLane &netserverLane, mbus::Entity base_entity, std::shared_ptr<nic_core::Nic> dev) {
	// Register this NIC with netserver
	managarm::net::RegisterNicRequest req;
	req.set_type(managarm::net::NicType::HARDWARE);
	req.set_mac(dev->mac);
	req.set_mtu(1500);

	auto [local_send_packets_lane, remote_send_packets_lane] = helix::createStream();

	auto [offer, sendReq, sendLane, recvResp, recvLane] = co_await helix_ng::exchangeMsgs(
		netserverLane,
		helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remote_send_packets_lane),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);

	// TODO: error handling
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(sendLane.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(recvLane.error());

	co_await dev->startDevice(recvLane.descriptor(), std::move(local_send_packets_lane));

	co_return;
}

} // namespace nic_Core
