
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

void OpenFile::seek(int64_t rel_offset, frigg::CallbackPtr<void()> callback) {
	auto closure = frigg::construct<SeekClosure>(*allocator,
			connection, externFd, rel_offset, callback);
	(*closure)();
}

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

MountPoint::MountPoint(helx::Pipe pipe)
: p_pipe(frigg::move(pipe)) { }

void MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	auto closure = frigg::construct<OpenClosure>(*allocator, *this, path, callback);
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

OpenClosure::OpenClosure(MountPoint &connection, frigg::StringView path,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback)
: connection(connection), path(path), callback(callback) { }

void OpenClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::OPEN);
	request.set_path(frigg::String<Allocator>(*allocator, path));

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	connection.getPipe().sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	// FIXME: fix request id
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &OpenClosure::recvResponse)));
}

void OpenClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	if(response.error() == managarm::fs::Errors::SUCCESS) {
		auto open_file = frigg::makeShared<OpenFile>(*allocator, connection, response.fd());
		callback(frigg::staticPtrCast<VfsOpenFile>(open_file));
	}else{
		assert(response.error() == managarm::fs::Errors::FILE_NOT_FOUND);
		callback(StdSharedPtr<VfsOpenFile>());
	}

	frigg::destruct(*allocator, this);
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

SeekClosure::SeekClosure(MountPoint &connection, int extern_fd, int64_t rel_offset,
		frigg::CallbackPtr<void()> callback)
: connection(connection), externFd(extern_fd), relOffset(rel_offset), callback(callback) { }

void SeekClosure::operator() () {
	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::fs::CntReqType::SEEK);
	request.set_fd(externFd);
	request.set_rel_offset(relOffset);

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
	callback();

	frigg::destruct(*allocator, this);
}


} // namespace extern_fs

