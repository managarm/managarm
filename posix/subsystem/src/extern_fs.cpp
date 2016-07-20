
#include "common.hpp"
#include "extern_fs.hpp"

#include <frigg/chain-all.hpp>

#include <fs.frigg_pb.hpp>

namespace extern_fs {

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(MountPoint &connection, int extern_fd)
: connection(connection), externFd(extern_fd) { }

void OpenFile::fstat(frigg::CallbackPtr<void(FileStats)> complete) {
	auto action = frigg::compose([this] (frigg::String<Allocator> *buffer) {
		buffer->resize(128);

		return frigg::compose([=] () {		
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::fs::CntReqType::FSTAT);
			request.set_fd(externFd);

			frigg::String<Allocator> serialized(*allocator);
			request.SerializeToString(&serialized);
			
			frigg::infoLogger() << "[posix/subsystem/src/extern-fs] OpenFile:fstat sendStringReq" << frigg::endLog;
			return connection.getPipe().sendStringReq(serialized.data(), serialized.size(),
					eventHub, 1, 0)
			+ frigg::compose([=] (HelError error) {
				HEL_CHECK(error);
				// FIXME: fix request id
				return connection.getPipe().recvStringResp(buffer->data(), 128, eventHub, 1, 0);
			});
		})
		+ frigg::lift([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
			HEL_CHECK(error);

			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(buffer->data(), length);

			assert(response.error() == managarm::fs::Errors::SUCCESS);

			FileStats stats;
			stats.inodeNumber = response.inode_num();
			stats.mode = response.mode();
			stats.numLinks = response.num_links();
			stats.uid = response.uid();
			stats.gid = response.gid();
			stats.fileSize = response.file_size();
			stats.atimeSecs = response.atime_secs();
			stats.atimeNanos = response.atime_nanos();
			stats.mtimeSecs = response.mtime_secs();
			stats.mtimeNanos = response.mtime_nanos();
			stats.ctimeSecs = response.ctime_secs();
			stats.ctimeNanos = response.ctime_nanos();
			
			return stats;
		});
	}, frigg::String<Allocator>(*allocator));

	frigg::run(action, allocator.get(), complete);
}

void OpenFile::connect(frigg::CallbackPtr<void()> complete) {
	auto action = frigg::compose([this] (frigg::String<Allocator> *buffer) {
		buffer->resize(128);
	
		 return frigg::compose([=] () {	
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::fs::CntReqType::CONNECT);
			request.set_fd(externFd);

			frigg::String<Allocator> serialized(*allocator);
			request.SerializeToString(&serialized);
			frigg::infoLogger() << "[posix/subsystem/src/extern-fs] OpenFile:connect sendStringReq" << frigg::endLog;
			return connection.getPipe().sendStringReq(serialized.data(), serialized.size(),
					eventHub, 1, 0)
			+frigg::compose([=] (HelError error) {
				HEL_CHECK(error);
				// FIXME: fix request id
				return connection.getPipe().recvStringResp(buffer->data(), 128, eventHub, 1, 0);
			});

		})
		+ frigg::lift([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
			HEL_CHECK(error);

			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(buffer->data(), length);

			assert(response.error() == managarm::fs::Errors::SUCCESS);
		});
	}, frigg::String<Allocator>(*allocator));

	frigg::run(action, allocator.get(), complete);
}

void OpenFile::write(const void *buffer, size_t size, frigg::CallbackPtr<void()> complete) {
	auto action = frigg::compose([this, buffer, size] (frigg::String<Allocator> *respBuffer) {
		respBuffer->resize(128);

		return frigg::compose([=] () {		
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::fs::CntReqType::WRITE);
			request.set_fd(externFd);
			request.set_size(size);

			frigg::String<Allocator> serialized(*allocator);
			request.SerializeToString(&serialized);
			frigg::infoLogger() << "[posix/subsystem/src/extern-fs] OpenFile:write sendStringReq" << frigg::endLog;
			return connection.getPipe().sendStringReq(serialized.data(), serialized.size(),
					eventHub, 1, 0)
			+ frigg::compose([=] (HelError error) {
				HEL_CHECK(error);
				return connection.getPipe().sendStringReq(buffer, size,
						eventHub, 1, 1)
				+ frigg::compose([=] (HelError error) {
					HEL_CHECK(error);
					// FIXME: fix request id
					return connection.getPipe().recvStringResp(respBuffer->data(), 128, eventHub, 1, 0);
				});
			});
		})
		+ frigg::lift([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
			HEL_CHECK(error);

			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(respBuffer->data(), length);

			assert(response.error() == managarm::fs::Errors::SUCCESS);
		});
	}, frigg::String<Allocator>(*allocator));

	frigg::run(action, allocator.get(), complete);
}

void OpenFile::read(void *buffer, size_t max_length, frigg::CallbackPtr<void(VfsError, size_t)> complete) {
	// work around a segfault in GCC
	auto max_length2 = max_length;
	auto action = frigg::compose([=] (frigg::String<Allocator> *respBuffer) {
		respBuffer->resize(128);

		return frigg::compose([=] (frigg::String<Allocator> *serialized) {
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::fs::CntReqType::READ);
			request.set_fd(externFd);
			request.set_size(max_length);
			request.SerializeToString(serialized);
			
			return connection.getPipe().sendStringReq(serialized->data(), serialized->size(), eventHub, 1, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator))
			// FIXME: fix request id
		+ connection.getPipe().recvStringResp(respBuffer->data(), 128, eventHub, 1, 0)
		+ frigg::compose([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
			HEL_CHECK(error);
			
			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(respBuffer->data(), length);

			return frigg::ifThenElse(
				frigg::lift([=] () { return response.error() == managarm::fs::Errors::END_OF_FILE; }),

				frigg::lift([=] () {
					complete(kVfsEndOfFile, 0);
				}),

				frigg::compose([=] () {
					assert(response.error() == managarm::fs::Errors::SUCCESS);
					// FIXME: fix request id
					return connection.getPipe().recvStringResp(buffer, max_length2, eventHub, 1, 1);
				})
				+ frigg::lift([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
					HEL_CHECK(error);

					complete(kVfsSuccess, length);
				})
			);
		});
	 }, frigg::String<Allocator>(*allocator));

	 frigg::run(action, allocator.get());
}

void OpenFile::mmap(frigg::CallbackPtr<void(HelHandle)> complete) {
	auto action = frigg::compose([this] (frigg::String<Allocator> *buffer) {
		buffer->resize(128);

		return frigg::compose([=] (frigg::String<Allocator> *serialized) {		
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::fs::CntReqType::MMAP);
			request.set_fd(externFd);
			request.SerializeToString(serialized);

			return connection.getPipe().sendStringReq(serialized->data(), serialized->size(), eventHub, 1, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator))
		// FIXME: fix request id
		+ connection.getPipe().recvStringResp(buffer->data(), 128, eventHub, 1, 0)
		+ frigg::await<void(HelError, int64_t, int64_t, HelHandle)>(
				[this, buffer] (auto callback, HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {		
			HEL_CHECK(error);

			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(buffer->data(), length);
			assert(response.error() == managarm::fs::Errors::SUCCESS);
			
			// FIXME: fix request id
			connection.getPipe().recvDescriptorResp(eventHub, 1, 1, callback);
		})
		+ frigg::lift([this] (HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle ) {	
			HEL_CHECK(error);

			return handle;
		});
	}, frigg::String<Allocator>(*allocator));

	frigg::run(action, allocator.get(), complete);
}

void OpenFile::seek(int64_t rel_offset, VfsSeek whence,	frigg::CallbackPtr<void(uint64_t)> complete) {
	auto action = frigg::compose([this, rel_offset, whence] (frigg::String<Allocator> *buffer) {
		buffer->resize(128);
		
		return frigg::compose([=] (frigg::String<Allocator> *serialized) {
			managarm::fs::CntRequest<Allocator> request(*allocator);
			request.set_fd(externFd);
			request.set_rel_offset(rel_offset);

			if(whence == kSeekAbs) {
				request.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
			}else if(whence == kSeekRel) {
				request.set_req_type(managarm::fs::CntReqType::SEEK_REL);
			}else if(whence == kSeekEof) {
				request.set_req_type(managarm::fs::CntReqType::SEEK_EOF);
			}else{
				frigg::panicLogger() << "Illegal whence argument" << frigg::endLog;
			}

			request.SerializeToString(serialized);
			
			return connection.getPipe().sendStringReq(serialized->data(), serialized->size(), eventHub, 1, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator))
		// FIXME: fix request id
		+ connection.getPipe().recvStringResp(buffer->data(), 128, eventHub, 1, 0)
		+ frigg::lift([=] (HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {	
			HEL_CHECK(error);

			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.ParseFromArray(buffer->data(), length);

			assert(response.error() == managarm::fs::Errors::SUCCESS);
			
			return response.offset();
		});
	}, frigg::String<Allocator>(*allocator));

	frigg::run(action, allocator.get(), complete);
}
// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

MountPoint::MountPoint(helx::Pipe pipe)
: p_pipe(frigg::move(pipe)) { }

void MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	auto closure = frigg::construct<OpenClosure>(*allocator, *this, frigg::move(path), callback);
	(*closure)();
}

helx::Pipe &MountPoint::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

OpenClosure::OpenClosure(MountPoint &connection, frigg::String<Allocator> path,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback)
: connection(connection), path(frigg::move(path)), callback(callback),
		linkTarget(*allocator) { }

void OpenClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::OPEN);
	request.set_path(frigg::String<Allocator>(*allocator, path));

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	// FIXME: fix this with a chain
	HelError send_closure_error;
	connection.getPipe().sendStringReqSync(serialized.data(), serialized.size(), eventHub, 1, 0, send_closure_error);
	HEL_CHECK(send_closure_error);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &OpenClosure::recvOpenResponse)));
}

void OpenClosure::recvOpenResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	if(response.error() == managarm::fs::Errors::FILE_NOT_FOUND) {
		callback(StdSharedPtr<VfsOpenFile>());
		frigg::destruct(*allocator, this);
		return;
	}
	assert(response.error() == managarm::fs::Errors::SUCCESS);
	
	if(response.file_type() == managarm::fs::FileType::REGULAR
			|| response.file_type() == managarm::fs::FileType::SOCKET
			|| response.file_type() == managarm::fs::FileType::DIRECTORY) {
		auto open_file = frigg::makeShared<OpenFile>(*allocator, connection, response.fd());
		callback(frigg::staticPtrCast<VfsOpenFile>(open_file));
		frigg::destruct(*allocator, this);
	}else{
		assert(response.file_type() == managarm::fs::FileType::SYMLINK);
		externFd = response.fd();

		// read the symlink target
		managarm::fs::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::fs::CntReqType::READ);
		request.set_fd(externFd);
		request.set_size(128);

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		frigg::infoLogger() << "[posix/subsystem/src/extern-fs] OpenClosure:recvOpenResponse sendStringReq" << frigg::endLog;
		connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
		
		// FIXME: fix request id
		HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
				CALLBACK_MEMBER(this, &OpenClosure::recvReadResponse)));
	}
}

void OpenClosure::recvReadResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);
	
	if(response.error() == managarm::fs::Errors::END_OF_FILE) {
		frigg::StringView path_view = path;
		size_t slash = path_view.findLast('/');
		assert(slash != size_t(-1));

		frigg::String<Allocator> target(*allocator, path_view.subString(0, slash + 1));
		target += linkTarget;
	
		auto closure = frigg::construct<OpenClosure>(*allocator, connection,
				frigg::move(target), callback);
		(*closure)();
		
		frigg::destruct(*allocator, this);
		return;
	}
	assert(response.error() == managarm::fs::Errors::SUCCESS);

	HEL_CHECK(connection.getPipe().recvStringResp(dataBuffer, 128, eventHub, 1, 1,
			CALLBACK_MEMBER(this, &OpenClosure::recvReadData)));
}

void OpenClosure::recvReadData(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);
	linkTarget += frigg::StringView(dataBuffer, length);

	// continue reading the symlink target
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::READ);
	request.set_fd(externFd);
	request.set_size(128);

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	frigg::infoLogger() << "[posix/subsystem/src/extern-fs] OpenClosure:recvReadData sendStringReq" << frigg::endLog;
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &OpenClosure::recvReadResponse)));
}

} // namespace extern_fs

