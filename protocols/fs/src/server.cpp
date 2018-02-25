
#include <stdio.h>
#include <string.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include <protocols/fs/server.hpp>
#include "fs.pb.h"

namespace protocols {
namespace fs {

namespace {

COFIBER_ROUTINE(cofiber::no_future, handlePassthrough(std::shared_ptr<void> file,
		const FileOperations *file_ops,
		managarm::fs::CntRequest req, helix::UniqueLane conversation),
		([file, file_ops, req = std::move(req), conversation = std::move(conversation)] () mutable {
	if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
		helix::SendBuffer send_resp;

		assert(file_ops->seekAbs);
		auto offset = COFIBER_AWAIT(file_ops->seekAbs(file, req.rel_offset()));
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(offset);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->seekRel);
		auto offset = COFIBER_AWAIT(file_ops->seekRel(file, req.rel_offset()));
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(offset);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->seekEof);
		auto offset = COFIBER_AWAIT(file_ops->seekEof(file, req.rel_offset()));
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(offset);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::READ) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;
		
		std::string data;
		data.resize(req.size());
		assert(file_ops->read);
		auto res = COFIBER_AWAIT(file_ops->read(file, &data[0], req.size()));
		
		managarm::fs::SvrResponse resp;
		auto error = std::get_if<Error>(&res);
		if(error && *error == Error::wouldBlock) {
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, data.data(), std::get<size_t>(res)));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}
	}else if(req.req_type() == managarm::fs::CntReqType::WRITE) {
		helix::RecvInline recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer));
		COFIBER_AWAIT buff.async_wait();
		HEL_CHECK(recv_buffer.error());
	
		assert(file_ops->write);
		COFIBER_AWAIT(file_ops->write(file, recv_buffer.data(), recv_buffer.length()));
		
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_READ_ENTRIES) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->readEntries);
		auto result = COFIBER_AWAIT(file_ops->readEntries(file));
		
		managarm::fs::SvrResponse resp;
		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_path(std::move(*result));
		}else{
			resp.set_error(managarm::fs::Errors::END_OF_FILE);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
		helix::SendBuffer send_resp;
		helix::PushDescriptor push_memory;
		
		// TODO: Fix the size.
		assert(file_ops->accessMemory);
		auto memory = COFIBER_AWAIT(file_ops->accessMemory(file, req.rel_offset(), 0));
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(memory.second);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_memory, memory.first));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(push_memory.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_TRUNCATE) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->truncate);
		COFIBER_AWAIT file_ops->truncate(file, req.size());
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_FALLOCATE) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->fallocate);
		COFIBER_AWAIT file_ops->fallocate(file, req.rel_offset(), req.size());
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_IOCTL) {
		assert(file_ops->ioctl);
		COFIBER_AWAIT file_ops->ioctl(file, std::move(req), std::move(conversation));
	}else if(req.req_type() == managarm::fs::CntReqType::FILE_POLL) {
		helix::SendBuffer send_resp;
		
		assert(file_ops->poll);
		auto result = COFIBER_AWAIT(file_ops->poll(file, req.sequence()));
		
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_sequence(std::get<0>(result));
		resp.set_edges(std::get<1>(result));
		resp.set_status(std::get<2>(result));

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		throw std::runtime_error("libfs_protocol: Unexpected"
				" request type in servePassthrough()");
	}
}))

} // anonymous namespace

COFIBER_ROUTINE(cofiber::no_future, servePassthrough(helix::UniqueLane p, std::shared_ptr<void> file,
		const FileOperations *file_ops), ([lane = std::move(p), file, file_ops] {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		handlePassthrough(file, file_ops, std::move(req), std::move(conversation));
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serveNode(helix::UniqueLane p, std::shared_ptr<void> node,
		const NodeOperations *node_ops, const FileOperations *file_ops),
		([lane = std::move(p), node, node_ops, file_ops] {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
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
			if(std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)),
						node_ops, file_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result));
				switch(std::get<2>(result)) {
				case FileType::directory:
					resp.set_file_type(managarm::fs::FileType::DIRECTORY);
					break;
				case FileType::regular:
					resp.set_file_type(managarm::fs::FileType::REGULAR);
					break;
				case FileType::symlink:
					resp.set_file_type(managarm::fs::FileType::SYMLINK);
					break;
				default:
					throw std::runtime_error("Unexpected file type");
				}

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&push_node, remote_lane));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
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
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_READ_SYMLINK) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_link;
			
			auto link = COFIBER_AWAIT node_ops->readSymlink(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_link, link.data(), link.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_link.error());
		}else{
			throw std::runtime_error("libfs_protocol: Unexpected request type in serveNode");
		}
	}
}))

} } // namespace protocols::fs

