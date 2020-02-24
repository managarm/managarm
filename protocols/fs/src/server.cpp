
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <vector>

#include <helix/ipc.hpp>

#include <protocols/fs/server.hpp>
#include "fs.pb.h"

namespace protocols {
namespace fs {

namespace {

async::detached handlePassthrough(smarter::shared_ptr<void> file,
		const FileOperations *file_ops,
		managarm::fs::CntRequest req, helix::UniqueLane conversation) {
	if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
		helix::SendBuffer send_resp;

		assert(file_ops->seekAbs);
		auto result = co_await file_ops->seekAbs(file.get(), req.rel_offset());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(std::get<int64_t>(result));

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
		helix::SendBuffer send_resp;

		assert(file_ops->seekRel);
		auto result = co_await file_ops->seekRel(file.get(), req.rel_offset());
		auto error = std::get_if<Error>(&result);

		managarm::fs::SvrResponse resp;
		if(error && *error == Error::seekOnPipe) {
			resp.set_error(managarm::fs::Errors::SEEK_ON_PIPE);
		}else{
			assert(!error);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_offset(std::get<int64_t>(result));
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		helix::SendBuffer send_resp;

		assert(file_ops->seekEof);
		auto result = co_await file_ops->seekEof(file.get(), req.rel_offset());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(std::get<int64_t>(result));

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::READ) {
		helix::ExtractCredentials extract_creds;
		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;

		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&extract_creds));
		co_await buff.async_wait();
		HEL_CHECK(extract_creds.error());

		std::string data;
		data.resize(req.size());
		assert(file_ops->read);
		auto res = co_await file_ops->read(file.get(), extract_creds.credentials(),
				data.data(), req.size());

		managarm::fs::SvrResponse resp;
		auto error = std::get_if<Error>(&res);
		if(error && *error == Error::wouldBlock) {
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(error && *error == Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			assert(!error);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, data.data(), std::get<size_t>(res)));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}
	}else if(req.req_type() == managarm::fs::CntReqType::WRITE) {
		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		helix::ExtractCredentials extract_creds;
		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&extract_creds, kHelItemChain),
				helix::action(&recv_buffer, buffer.data(), buffer.size()));
		co_await buff.async_wait();
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_buffer.error());

		assert(file_ops->write);
		co_await file_ops->write(file.get(), extract_creds.credentials(),
				buffer.data(), recv_buffer.actualLength());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::FLOCK) {
		helix::SendBuffer send_resp;

		assert(file_ops->flock);
		auto result = co_await file_ops->flock(file.get(), req.flock_flags());

		managarm::fs::SvrResponse resp;
		if(result == protocols::fs::Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else if(result == protocols::fs::Error::wouldBlock) {
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_READ_ENTRIES) {
		helix::SendBuffer send_resp;

		assert(file_ops->readEntries);
		auto result = co_await file_ops->readEntries(file.get());

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
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
		helix::SendBuffer send_resp;
		helix::PushDescriptor push_memory;

		// TODO: Fix the size.
		assert(file_ops->accessMemory);
		auto memory = co_await file_ops->accessMemory(file.get(), req.rel_offset(), 0);
		assert(!memory.second);

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(memory.second);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_memory, memory.first));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(push_memory.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_TRUNCATE) {
		helix::SendBuffer send_resp;

		assert(file_ops->truncate);
		co_await file_ops->truncate(file.get(), req.size());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_FALLOCATE) {
		helix::SendBuffer send_resp;

		assert(file_ops->fallocate);
		co_await file_ops->fallocate(file.get(), req.rel_offset(), req.size());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_IOCTL) {
		assert(file_ops->ioctl);
		co_await file_ops->ioctl(file.get(), std::move(req), std::move(conversation));
	}else if(req.req_type() == managarm::fs::CntReqType::PT_GET_OPTION) {
		helix::SendBuffer send_resp;

		assert(file_ops->getOption);
		auto result = co_await file_ops->getOption(file.get(), req.command());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_pid(result);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_SET_OPTION) {
		helix::SendBuffer send_resp;

		assert(file_ops->setOption);
		co_await file_ops->setOption(file.get(), req.command(), req.value());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::FILE_POLL) {
		helix::PullDescriptor pull_cancel;
		helix::SendBuffer send_resp;

		auto &&receive = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&pull_cancel));
		co_await receive.async_wait();
		HEL_CHECK(pull_cancel.error());

		assert(file_ops->poll);
		auto result = co_await file_ops->poll(file.get(), req.sequence(),
				async::cancellation_token{});

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_sequence(std::get<0>(result));
		resp.set_edges(std::get<1>(result));
		resp.set_status(std::get<2>(result));

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_BIND) {
		helix::ExtractCredentials extract_creds;
		helix::RecvInline recv_addr;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&extract_creds, kHelItemChain),
				helix::action(&recv_addr));
		co_await buff.async_wait();
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		assert(file_ops->bind);
		co_await file_ops->bind(file.get(), extract_creds.credentials(),
				recv_addr.data(), recv_addr.length());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_CONNECT) {
		helix::ExtractCredentials extract_creds;
		helix::RecvInline recv_addr;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&extract_creds, kHelItemChain),
				helix::action(&recv_addr));
		co_await buff.async_wait();
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		assert(file_ops->connect);
		co_await file_ops->connect(file.get(), extract_creds.credentials(),
				recv_addr.data(), recv_addr.length());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_SOCKNAME) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;

		std::vector<char> addr;
		addr.resize(req.size());
		assert(file_ops->sockname);
		auto actual_length = co_await file_ops->sockname(file.get(),
				addr.data(), req.size());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_file_size(actual_length);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_data, addr.data(),
						std::min(size_t(req.size()), actual_length)));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_GET_FILE_FLAGS) {
		helix::SendBuffer send_resp;

		auto flags = co_await file_ops->getFileFlags(file.get());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_flags(flags);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), 0));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.req_type() == managarm::fs::CntReqType::PT_SET_FILE_FLAGS) {
		helix::SendBuffer send_resp;

		co_await file_ops->setFileFlags(file.get(), req.flags());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), 0));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	} else if (req.req_type() == managarm::fs::CntReqType::PT_RECVMSG) {
		helix::ExtractCredentials extract_creds;
		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;
		helix::SendBuffer send_addr;
		helix::SendBuffer send_ctrl;

		auto get_creds = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&extract_creds));
		co_await get_creds.async_wait();
		HEL_CHECK(extract_creds.error());

		// ensure we have a handler
		assert(file_ops->recvMsg);
		std::vector<char> buffer;
		buffer.resize(req.size());
		std::vector<char> addr;
		addr.resize(req.addr_size());

		auto result = co_await file_ops->recvMsg(file.get(), extract_creds.credentials(), req.flags(),
				buffer.data(), buffer.size(),
				addr.data(), addr.size(),
				req.ctrl_size());
		auto error = std::get_if<Error>(&result);
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::SUCCESS);

		if (error) {
			assert(*error == Error::wouldBlock && "libfs_protocol: TODO: handle other errors");
			resp.set_error(managarm::fs::WOULD_BLOCK);

			auto ser = resp.SerializeAsString();
			auto transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto data = std::get<RecvData>(result);
		auto ser = resp.SerializeAsString();
		auto transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_addr, addr.data(), data.addressLength, kHelItemChain),
				helix::action(&send_data, buffer.data(), data.dataLength, kHelItemChain),
				helix::action(&send_ctrl, data.ctrl.data(), data.ctrl.size()));

		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_addr.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(send_ctrl.error());
	} else if (req.req_type() == managarm::fs::CntReqType::PT_SENDMSG) {
		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		helix::RecvBuffer recv_data;
		helix::ExtractCredentials extract_creds;
		helix::RecvInline recv_addr;
		helix::SendBuffer send_resp;

		auto &&submit_data = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_data, buffer.data(), buffer.size(), kHelItemChain),
				helix::action(&extract_creds, kHelItemChain),
				helix::action(&recv_addr));
		co_await submit_data.async_wait();
		HEL_CHECK(recv_data.error());
		HEL_CHECK(extract_creds.error());


		std::vector<uint32_t> files(req.fds().cbegin(), req.fds().cend());

		auto result_or_error = co_await file_ops->sendMsg(file.get(),
				extract_creds.credentials(), req.flags(),
				buffer.data(), recv_data.actualLength(),
				recv_addr.data(), recv_addr.length(),
				std::move(files));

		managarm::fs::SvrResponse resp;

		auto error = std::get_if<Error>(&result_or_error);
		if(error && *error == Error::brokenPipe) {
			resp.set_error(managarm::fs::Errors::BROKEN_PIPE);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			co_return;
		} else {
			assert(!error);
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_size(std::get<size_t>(result_or_error));

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	} else {
		throw std::runtime_error("libfs_protocol: Unexpected"
				" request type in servePassthrough()");
	}
}

} // anonymous namespace

async::result<void>
serveFile(helix::UniqueLane lane, void *file, const FileOperations *file_ops) {
	(void)file;
	(void)file_ops;
	while(true) {
		helix::Accept accept;

		auto &&initiate = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept));
		co_await initiate.async_wait();

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		assert(!"No operations are defined yet for the non-passthrough protocol");
	}
}

async::result<void> servePassthrough(helix::UniqueLane lane,
		smarter::shared_ptr<void> file, const FileOperations *file_ops,
		async::cancellation_token cancellation) {
	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(lane.getHandle()));
	}};

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&initiate = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await initiate.async_wait();

		// TODO: Handle end-of-lane correctly. Why does it even happen here?
		if(accept.error() == kHelErrLaneShutdown
				|| accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		handlePassthrough(file, file_ops, std::move(req), std::move(conversation));
	}
}

StatusPageProvider::StatusPageProvider() {
	// Allocate and map our stauts page.
	size_t page_size = 4096;
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(page_size, 0, nullptr, &handle));
	_memory = helix::UniqueDescriptor{handle};
	_mapping = helix::Mapping{_memory, 0, page_size};
}

void StatusPageProvider::update(uint64_t sequence, int status) {
	auto page = reinterpret_cast<protocols::fs::StatusPage *>(_mapping.get());

	// State the seqlock write.
	auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_RELAXED);
	assert(!(seqlock & 1));
	__atomic_store_n(&page->seqlock, seqlock + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);

	// Perform the actual update.
	__atomic_store_n(&page->sequence, sequence, __ATOMIC_RELAXED);
	__atomic_store_n(&page->status, status, __ATOMIC_RELAXED);

	// Complete the seqlock write.
	__atomic_store_n(&page->seqlock, seqlock + 2, __ATOMIC_RELEASE);
}

async::detached serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::NODE_GET_STATS) {
			helix::SendBuffer send_resp;

			assert(node_ops->getStats);
			auto result = co_await node_ops->getStats(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_file_size(result.fileSize);
			resp.set_num_links(result.linkCount);
			resp.set_mode(result.mode);
			resp.set_uid(result.uid);
			resp.set_gid(result.gid);
			resp.set_atime_secs(result.accessTime.tv_sec);
			resp.set_atime_nanos(result.accessTime.tv_nsec);
			resp.set_mtime_secs(result.dataModifyTime.tv_sec);
			resp.set_mtime_nanos(result.dataModifyTime.tv_nsec);
			resp.set_ctime_secs(result.anyChangeTime.tv_sec);
			resp.set_ctime_nanos(result.anyChangeTime.tv_nsec);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_GET_LINK) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;

			auto result = co_await node_ops->getLink(node, req.path());
			if(std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)), node_ops);

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
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_MKDIR) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;

			auto result = co_await node_ops->mkdir(node, req.path());

			if (std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)), node_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result));

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&push_node, remote_lane));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT); // TODO

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_LINK) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;

			auto result = co_await node_ops->link(node, req.path(), req.fd());
			if(std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)), node_ops);

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
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_UNLINK) {
			helix::SendBuffer send_resp;

			co_await node_ops->unlink(node, req.path());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_file;
			helix::PushDescriptor push_pt;

			auto result = co_await node_ops->open(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_file, std::get<0>(result), kHelItemChain),
					helix::action(&push_pt, std::get<1>(result)));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_file.error());
			HEL_CHECK(push_pt.error());
		}else if(req.req_type() == managarm::fs::CntReqType::NODE_READ_SYMLINK) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_link;

			auto link = co_await node_ops->readSymlink(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_link, link.data(), link.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_link.error());
		}else{
			throw std::runtime_error("libfs_protocol: Unexpected request type in serveNode");
		}
	}
}

} } // namespace protocols::fs

