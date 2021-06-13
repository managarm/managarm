
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <vector>

#include <helix/ipc.hpp>

#include <protocols/fs/server.hpp>
#include "fs.bragi.hpp"

namespace protocols {
namespace fs {

namespace {

async::detached handlePassthrough(smarter::shared_ptr<void> file,
		const FileOperations *file_ops,
		managarm::fs::CntRequest req, helix::UniqueLane conversation) {
	switch(req.req_type()) {
	case managarm::fs::CntReqType::SEEK_ABS: {
		if(!file_ops->seekAbs) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->seekAbs(file.get(), req.rel_offset());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(std::get<int64_t>(result));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::SEEK_REL: {
		if(!file_ops->seekRel) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
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
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::SEEK_EOF: {
		if(!file_ops->seekEof) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->seekEof(file.get(), req.rel_offset());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_offset(std::get<int64_t>(result));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::READ: {
		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->read) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::string data;
		data.resize(req.size());
		auto res = co_await file_ops->read(file.get(), extract_creds.credentials(),
				data.data(), req.size());

		managarm::fs::SvrResponse resp;
		auto error = std::get_if<Error>(&res);
		if(error && *error == Error::wouldBlock) {
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(error && *error == Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			assert(!error);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(data.data(), std::get<size_t>(res))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}
		break;
	}
	case managarm::fs::CntReqType::PT_PREAD: {
		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->pread) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::string data;
		data.resize(req.size());
		auto res = co_await file_ops->pread(file.get(), req.offset(), extract_creds.credentials(),
				data.data(), req.size());

		managarm::fs::SvrResponse resp;
		auto error = std::get_if<Error>(&res);
		if(error && *error == Error::wouldBlock) {
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(error && *error == Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			assert(!error);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(data.data(), std::get<size_t>(res))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}
		break;
	}
	case managarm::fs::CntReqType::WRITE: {
		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		auto [extract_creds, recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials(),
			helix_ng::recvBuffer(buffer.data(), buffer.size())
		);
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_buffer.error());

		if(!file_ops->write) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto res = co_await file_ops->write(file.get(), extract_creds.credentials(),
				buffer.data(), recv_buffer.actualLength());

		managarm::fs::SvrResponse resp;
		if(!res) {
			switch(res.error()) {
			case Error::noSpaceLeft:
				resp.set_error(managarm::fs::Errors::NO_SPACE_LEFT);
				break;
			case Error::wouldBlock:
				resp.set_error(managarm::fs::Errors::WOULD_BLOCK);
				break;
			default:
				std::cout << "Unknown error from write()" << std::endl;
				co_return;
			}
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_size(res.value());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}

		break;
	}
	case managarm::fs::CntReqType::FLOCK: {
		if(!file_ops->flock) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->flock(file.get(), req.flock_flags());

		managarm::fs::SvrResponse resp;
		switch(result) {
		case protocols::fs::Error::illegalArguments:
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			break;
		case protocols::fs::Error::wouldBlock:
			resp.set_error(managarm::fs::Errors::WOULD_BLOCK);
			break;
		default:
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_READ_ENTRIES: {
		if(!file_ops->readEntries) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->readEntries(file.get());

		managarm::fs::SvrResponse resp;
		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_path(std::move(*result));
		}else{
			resp.set_error(managarm::fs::Errors::END_OF_FILE);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::MMAP: {
		if(!file_ops->accessMemory) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto memory = co_await file_ops->accessMemory(file.get());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp, push_memory] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::pushDescriptor(memory)
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(push_memory.error());
		break;
	}
	case managarm::fs::CntReqType::PT_TRUNCATE: {
		if(!file_ops->truncate) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->truncate(file.get(), req.size());

		managarm::fs::SvrResponse resp;
		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else{
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_FALLOCATE: {
		if(!file_ops->fallocate) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		co_await file_ops->fallocate(file.get(), req.rel_offset(), req.size());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_IOCTL: {
		if(!file_ops->ioctl) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		co_await file_ops->ioctl(file.get(), std::move(req), std::move(conversation));
		break;
	}
	case managarm::fs::CntReqType::PT_GET_OPTION: {
		if(!file_ops->getOption) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto result = co_await file_ops->getOption(file.get(), req.command());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_pid(result);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_SET_OPTION: {
		if(!file_ops->setOption) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		co_await file_ops->setOption(file.get(), req.command(), req.value());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::FILE_POLL_WAIT: {
		auto [pull_cancel] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::pullDescriptor()
		);
		HEL_CHECK(pull_cancel.error());

		if(!file_ops->pollWait) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto resultOrError = co_await file_ops->pollWait(file.get(),
				req.sequence(), req.event_mask(),
				async::cancellation_token{});
		if(!resultOrError) {
			managarm::fs::SvrResponse resp;
			resp.set_error(static_cast<managarm::fs::Errors>(resultOrError.error()));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto result = resultOrError.value();

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_sequence(std::get<0>(result));
		resp.set_edges(std::get<1>(result));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::FILE_POLL_STATUS: {
		if(!file_ops->pollStatus) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto resultOrError = co_await file_ops->pollStatus(file.get());
		if(!resultOrError) {
			managarm::fs::SvrResponse resp;
			resp.set_error(static_cast<managarm::fs::Errors>(resultOrError.error()));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto result = resultOrError.value();

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_sequence(std::get<0>(result));
		resp.set_status(std::get<1>(result));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_BIND: {
		auto [extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials(),
			helix_ng::recvInline()
		);
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		if(!file_ops->bind) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto error = co_await file_ops->bind(file.get(),
			extract_creds.credentials(),
			recv_addr.data(), recv_addr.length());

		managarm::fs::SvrResponse resp;
		resp.set_error(static_cast<managarm::fs::Errors>(error));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_CONNECT: {
		auto [extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials(),
			helix_ng::recvInline()
		);
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		if(!file_ops->connect) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto error = co_await file_ops->connect(file.get(),
			extract_creds.credentials(),
			recv_addr.data(), recv_addr.length());

		managarm::fs::SvrResponse resp;
		resp.set_error(static_cast<managarm::fs::Errors>(error));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_SOCKNAME: {
		if(!file_ops->sockname) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<char> addr;
		addr.resize(req.size());
		auto actual_length = co_await file_ops->sockname(file.get(),
				addr.data(), req.size());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_file_size(actual_length);

		auto ser = resp.SerializeAsString();
		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(addr.data(),
					std::min(size_t(req.size()), actual_length))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
		break;
	}
	case managarm::fs::CntReqType::PT_PEERNAME: {
		if(!file_ops->peername) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<char> addr;
		addr.resize(req.size());
		auto result = co_await file_ops->peername(file.get(), addr.data(), req.size());

		if (!result) {
			managarm::fs::SvrResponse resp;
			resp.set_error(static_cast<managarm::fs::Errors>(result.error()));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			co_return;
		}

		assert(result); // This can never fail (yet)
		auto actual_length = result.value();

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_file_size(actual_length);

		auto ser = resp.SerializeAsString();
		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(addr.data(),
					std::min(size_t(req.size()), actual_length))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
		break;
	}
	case managarm::fs::CntReqType::PT_GET_FILE_FLAGS: {
		if(!file_ops->getFileFlags) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto flags = co_await file_ops->getFileFlags(file.get());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_flags(flags);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_SET_FILE_FLAGS: {
		if(!file_ops->setFileFlags) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		co_await file_ops->setFileFlags(file.get(), req.flags());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_LISTEN: {
		if(!file_ops->listen) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		co_await file_ops->listen(file.get());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case managarm::fs::CntReqType::PT_RECVMSG: {
		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->recvMsg) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<char> buffer;
		buffer.resize(req.size());
		std::vector<char> addr;
		addr.resize(req.addr_size());

		auto result = co_await file_ops->recvMsg(file.get(),
			extract_creds.credentials(), req.flags(),
			buffer.data(), buffer.size(),
			addr.data(), addr.size(),
			req.ctrl_size());
		auto error = std::get_if<Error>(&result);
		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		if (error) {
			resp.set_error(static_cast<managarm::fs::Errors>(*error));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto data = std::get<RecvData>(result);
		resp.set_addr_size(data.addressLength);
		auto ser = resp.SerializeAsString();
		auto [send_resp, send_addr, send_data, send_ctrl]
				= co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(addr.data(),
				std::min(addr.size(), data.addressLength)),
			helix_ng::sendBuffer(buffer.data(), data.dataLength),
			helix_ng::sendBuffer(data.ctrl.data(), data.ctrl.size())
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_addr.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(send_ctrl.error());
		break;
	}
	case managarm::fs::CntReqType::PT_SENDMSG: {
		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		auto [recv_data, extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(buffer.data(), buffer.size()),
			helix_ng::extractCredentials(),
			helix_ng::recvInline()
		);
		HEL_CHECK(recv_data.error());
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		if(!file_ops->sendMsg) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<uint32_t> files(req.fds().cbegin(), req.fds().cend());

		auto res = co_await file_ops->sendMsg(file.get(),
				extract_creds.credentials(), req.flags(),
				buffer.data(), recv_data.actualLength(),
				recv_addr.data(), recv_addr.length(),
				std::move(files));

		managarm::fs::SvrResponse resp;

		if(!res) {
			if(res.error() == Error::wouldBlock) {
				resp.set_error(managarm::fs::Errors::WOULD_BLOCK);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			} else {
				std::cout << "Unknown error from sendMsg()" << std::endl;
				co_return;
			}
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_size(res.value());

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	default:
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
		auto [accept] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept()
		);

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
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);

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
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		switch(req.req_type()) {
		case managarm::fs::CntReqType::NODE_GET_STATS: {
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
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_GET_LINK: {
			auto result = co_await node_ops->getLink(node, req.path());
			if(!result) {
				managarm::fs::SvrResponse resp;
				assert(result.error() == protocols::fs::Error::notDirectory);
				resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);
				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				continue;
			}

			if(std::get<0>(result.value())) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result.value()));
				switch(std::get<2>(result.value())) {
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
				auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			}
			break;
		}
		case managarm::fs::CntReqType::NODE_TRAVERSE_LINKS: {
			auto result = co_await node_ops->traverseLinks(node, std::deque(req.path_segments().begin(), req.path_segments().end()));

			if (!result) {
				managarm::fs::SvrResponse resp;
				if (result.error() == protocols::fs::Error::notDirectory) {
					resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);
				} else {
					assert(result.error() == protocols::fs::Error::fileNotFound);
					resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
				}

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto [nodes, type, processedComponents] = result.value();

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_links_traversed(processedComponents);
			switch(type) {
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

			// TODO: this is a workaround for not being able to get the offer lane on the offer side
			helix::UniqueLane local_push, remote_push;
			std::tie(local_push, remote_push) = helix::createStream();

			for (auto &[_, id] : nodes) {
				resp.add_ids(id);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_desc] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_push)
			);

			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_desc.error());

			for (auto &[node, _] : nodes) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(node), node_ops);

				auto [push_node] = co_await helix_ng::exchangeMsgs(
					local_push,
					helix_ng::pushDescriptor(remote_lane)
				);

				HEL_CHECK(push_node.error());
			}
			break;
		}
		case managarm::fs::CntReqType::NODE_MKDIR: {
			auto result = co_await node_ops->mkdir(node, req.path());

			if (std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)), node_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result));

				auto ser = resp.SerializeAsString();
				auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT); // TODO

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			}
			break;
		}
		case managarm::fs::CntReqType::NODE_SYMLINK: {
			std::string name;
			std::string target;
			name.resize(req.name_length());
			target.resize(req.target_length());

			auto [recvName, recvTarget] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(name.data(), name.size()),
				helix_ng::recvBuffer(target.data(), target.size())
			);
			HEL_CHECK(recvName.error());
			HEL_CHECK(recvTarget.error());

			auto result = co_await node_ops->symlink(node, std::move(name), std::move(target));

			if (std::get<0>(result)) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result)), node_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result));

				auto ser = resp.SerializeAsString();
				auto [sendResp, pushNode] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
				HEL_CHECK(sendResp.error());
				HEL_CHECK(pushNode.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT); // TODO

				auto ser = resp.SerializeAsString();
				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(sendResp.error());
			}
			break;
		}
		case managarm::fs::CntReqType::NODE_LINK: {
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
				auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(push_node.error());
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			}
			break;
		}
		case managarm::fs::CntReqType::NODE_UNLINK: {
			auto result = co_await node_ops->unlink(node, req.path());
			managarm::fs::SvrResponse resp;
			if(!result) {
				assert(result.error() == protocols::fs::Error::fileNotFound);
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				continue;
			}
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_RMDIR: {
			// TODO: This should probably be it's own operation, for now, let it be
			auto result = co_await node_ops->unlink(node, req.path());
			managarm::fs::SvrResponse resp;
			if(!result) {
				assert(result.error() == protocols::fs::Error::fileNotFound);
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				continue;
			}
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_OPEN: {
			auto result = co_await node_ops->open(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_file, push_pt] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(std::get<0>(result)),
				helix_ng::pushDescriptor(std::get<1>(result))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_file.error());
			HEL_CHECK(push_pt.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_READ_SYMLINK: {
			auto link = co_await node_ops->readSymlink(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_link] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(link.data(), link.size())
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_link.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_CHMOD: {
			co_await node_ops->chmod(node, req.mode());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_UTIMENSAT: {
			co_await node_ops->utimensat(node, req.atime_sec(), req.atime_nsec(), req.mtime_sec(), req.mtime_nsec());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		case managarm::fs::CntReqType::NODE_OBSTRUCT_LINK: {
			co_await node_ops->obstructLink(node, req.link_name());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			break;
		}
		default:
			throw std::runtime_error("libfs_protocol: Unexpected request type in serveNode");
		}
	}
}

} } // namespace protocols::fs
