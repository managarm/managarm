
#include "common.hpp"
#include "extern_fs.hpp"

#include <fs.frigg_pb.hpp>

namespace extern_fs {

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(MountPoint &connection, int extern_fd)
: connection(connection), externFd(extern_fd) { }

void OpenFile::fstat(frigg::CallbackPtr<void(FileStats)> callback) {
	auto closure = frigg::construct<StatClosure>(*allocator,
			connection, externFd, callback);
	(*closure)();
}

void OpenFile::write(const void *buffer, size_t length, frigg::CallbackPtr<void()> callback) {
	assert(!"Not implemented");
}

void OpenFile::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(VfsError, size_t)> callback) {
	auto closure = frigg::construct<ReadClosure>(*allocator,
			connection, externFd, buffer, max_length, callback);
	(*closure)();
}

void OpenFile::mmap(frigg::CallbackPtr<void(HelHandle)> callback) {
	auto closure = frigg::construct<MapClosure>(*allocator,
			connection, externFd, callback);
	(*closure)();
}

void OpenFile::seek(int64_t rel_offset, VfsSeek whence,
		frigg::CallbackPtr<void(uint64_t)> callback) {
	auto closure = frigg::construct<SeekClosure>(*allocator,
			connection, externFd, rel_offset, whence, callback);
	(*closure)();
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
// StatClosure
// --------------------------------------------------------

StatClosure::StatClosure(MountPoint &connection, int extern_fd,
		frigg::CallbackPtr<void(FileStats)> callback)
: connection(connection), externFd(extern_fd), callback(callback) { }

void StatClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::FSTAT);
	request.set_fd(externFd);

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &StatClosure::recvResponse)));
}

void StatClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	assert(response.error() == managarm::fs::Errors::SUCCESS);

	FileStats stats;
	stats.fileSize = response.file_size();
	callback(stats);

	frigg::destruct(*allocator, this);
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
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
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
	
	if(response.file_type() == managarm::fs::FileType::REGULAR) {
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
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &OpenClosure::recvReadResponse)));
}

// --------------------------------------------------------
// ReadClosure
// --------------------------------------------------------

ReadClosure::ReadClosure(MountPoint &connection,
		int extern_fd, void *read_buffer, size_t max_size,
		frigg::CallbackPtr<void(VfsError, size_t)> callback)
: connection(connection), externFd(extern_fd), readBuffer(read_buffer), maxSize(max_size),
		callback(callback) { }

void ReadClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::READ);
	request.set_fd(externFd);
	request.set_size(maxSize);

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &ReadClosure::recvResponse)));
}

void ReadClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	if(response.error() == managarm::fs::Errors::END_OF_FILE) {
		callback(kVfsEndOfFile, 0);
		frigg::destruct(*allocator, this);
		return;
	}
	assert(response.error() == managarm::fs::Errors::SUCCESS);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(readBuffer, maxSize, eventHub, 1, 1,
			CALLBACK_MEMBER(this, &ReadClosure::recvData)));
}

void ReadClosure::recvData(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	callback(kVfsSuccess, length);
	frigg::destruct(*allocator, this);
}

// --------------------------------------------------------
// SeekClosure
// --------------------------------------------------------

SeekClosure::SeekClosure(MountPoint &connection,
		int extern_fd, int64_t rel_offset, VfsSeek whence,
		frigg::CallbackPtr<void(uint64_t)> callback)
: connection(connection), externFd(extern_fd), relOffset(rel_offset), whence(whence),
		callback(callback) { }

void SeekClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_fd(externFd);
	request.set_rel_offset(relOffset);
	
	if(whence == kSeekAbs) {
		request.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	}else if(whence == kSeekRel) {
		request.set_req_type(managarm::fs::CntReqType::SEEK_REL);
	}else if(whence == kSeekEof) {
		request.set_req_type(managarm::fs::CntReqType::SEEK_EOF);
	}else{
		frigg::panicLogger.log() << "Illegal whence argument" << frigg::EndLog();
	}

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &SeekClosure::recvResponse)));
}

void SeekClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	assert(response.error() == managarm::fs::Errors::SUCCESS);
	callback(response.offset());

	frigg::destruct(*allocator, this);
}

// --------------------------------------------------------
// MapClosure
// --------------------------------------------------------

MapClosure::MapClosure(MountPoint &connection, int extern_fd,
		frigg::CallbackPtr<void(HelHandle)> callback)
: connection(connection), externFd(extern_fd), callback(callback) { }

void MapClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::MMAP);
	request.set_fd(externFd);

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &MapClosure::recvResponse)));
}

void MapClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);
	assert(response.error() == managarm::fs::Errors::SUCCESS);
	
	// FIXME: fix request id
	connection.getPipe().recvDescriptorResp(eventHub, 1, 1,
			CALLBACK_MEMBER(this, &MapClosure::recvHandle));
}

void MapClosure::recvHandle(HelError error, int64_t msg_request, int64_t msg_seq,
		HelHandle file_memory) {
	HEL_CHECK(error);

	callback(file_memory);
	frigg::destruct(*allocator, this);
}

} // namespace extern_fs

