
#include <stdio.h>
#include <string.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include <protocols/fs/server.hpp>
#include "fs.pb.h"

namespace protocols {
namespace fs {

COFIBER_ROUTINE(cofiber::no_future, servePassthrough(helix::UniqueLane p, std::shared_ptr<void> file,
		const FileOperations *file_ops), ([lane = std::move(p), file, file_ops] {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
			helix::SendBuffer send_resp;
			
			COFIBER_AWAIT(file_ops->seek(file, req.rel_offset()));
			
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size()),
			}, helix::Dispatcher::global());
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::fs::CntReqType::READ) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			std::string data;
			data.resize(req.size());
			COFIBER_AWAIT(file_ops->read(file, &data[0], req.size()));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_data, data.data(), data.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_memory;
			
			auto memory = COFIBER_AWAIT(file_ops->accessMemory(file));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_memory, memory)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_memory.error());
		}else{
			throw std::runtime_error("libfs_protocol: Unexpected"
					" request type in servePassthrough()");
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serveNode(helix::UniqueLane p, std::shared_ptr<void> node,
		const NodeOperations *node_ops, const FileOperations *file_ops),
		([lane = std::move(p), node, node_ops, file_ops] {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::NODE_GET_LINK) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			auto result = COFIBER_AWAIT node_ops->getLink(node, req.path());
			assert(std::get<0>(result));

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(std::get<0>(result)),
					node_ops, file_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			switch(std::get<1>(result)) {
			case FileType::directory:
				resp.set_file_type(managarm::fs::FileType::DIRECTORY);
				break;
			case FileType::regular:
				resp.set_file_type(managarm::fs::FileType::REGULAR);
				break;
			default:
				throw std::runtime_error("Unexpected file type");
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_node, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			auto file = COFIBER_AWAIT node_ops->open(node);
			assert(file);

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			servePassthrough(std::move(local_lane), std::move(file), file_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_node, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("libfs_protocol: Unexpected request type in serveNode");
		}
	}
}))

} } // namespace protocols::fs

