
#include "common.hpp"
#include "extern_fs.hpp"

#include <fs.frigg_pb.hpp>

namespace extern_fs {

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(int extern_fd)
: externFd(extern_fd) { }

void OpenFile::write(const void *buffer, size_t length, frigg::CallbackPtr<void()> callback) {
	assert(!"Not implemented");
}

void OpenFile::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(size_t)> callback) {
	assert(!"Not implemented");
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
// Closures
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
	
	HEL_CHECK(connection.getPipe().recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &OpenClosure::recvResponse)));
}

void OpenClosure::recvResponse(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);

	if(response.error() == managarm::fs::Errors::SUCCESS) {
		auto open_file = frigg::makeShared<OpenFile>(*allocator, response.fd());
		callback(frigg::staticPtrCast<VfsOpenFile>(open_file));
	}else{
		assert(response.error() == managarm::fs::Errors::FILE_NOT_FOUND);
		callback(StdSharedPtr<VfsOpenFile>());
	}

	frigg::destruct(*allocator, this);
}

} // namespace extern_fs

