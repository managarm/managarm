
#include <stdio.h>
#include <string.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include <protocols/fs/server.hpp>
#include "fs.pb.h"

namespace protocols {
namespace fs {

COFIBER_ROUTINE(cofiber::no_future, serveNode(helix::UniqueLane p, std::shared_ptr<void> node,
		const NodeOperations *node_ops), ([lane = std::move(p), node, node_ops] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvInline<M> recv_req;

		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::NODE_GET_LINK) {
			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_node;
			
			auto child = COFIBER_AWAIT node_ops->getLink(node, req.path());
			assert(child);

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(child), node_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_node, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_node.future();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

} } // namespace protocols::fs

