
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/smart_ptr.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/optional.hpp>
#include <frigg/tuple.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/protobuf.hpp>

#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
#include "dev_fs.hpp"
#include "pts_fs.hpp"
#include "sysfile_fs.hpp"
#include "extern_fs.hpp"

#include "posix.frigg_pb.hpp"
#include "mbus.frigg_pb.hpp"

bool traceRequests = false;

helx::EventHub eventHub = helx::EventHub::create();
helx::Client mbusConnect;
helx::Pipe ldServerPipe;
helx::Pipe mbusPipe;

// TODO: this could be handled better
helx::Pipe initrdPipe;

// TODO: this is a ugly hack
MountSpace *initMountSpace;

void sendResponse(helx::Pipe &pipe, managarm::posix::ServerResponse<Allocator> &response,
		int64_t msg_request) {
	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	pipe.sendStringResp(serialized.data(), serialized.size(), msg_request, 0);
}

// --------------------------------------------------------
// StatClosure
// --------------------------------------------------------

struct StatClosure : frigg::BaseClosure<StatClosure> {
	StatClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void statComplete(FileStats stats);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

StatClosure::StatClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void StatClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	(*file)->fstat(CALLBACK_MEMBER(this, &StatClosure::statComplete));
}

void StatClosure::statComplete(FileStats stats) {
	if(traceRequests)
		infoLogger->log() << "[" << process->pid << "] FSTAT response" << frigg::EndLog();

	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	response.set_file_size(stats.fileSize);
	sendResponse(*pipe, response, msgRequest);

	suicide(*allocator);
}

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

struct OpenClosure : frigg::BaseClosure<OpenClosure> {
	OpenClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void openComplete(StdSharedPtr<VfsOpenFile> file);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

OpenClosure::OpenClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void OpenClosure::operator() () {
	uint32_t open_flags = 0;
	if((request.flags() & managarm::posix::OpenFlags::CREAT) != 0)
		open_flags |= MountSpace::kOpenCreat;

	uint32_t open_mode = 0;
	if((request.mode() & managarm::posix::OpenMode::HELFD) != 0)
		open_mode |= MountSpace::kOpenHelfd;

	MountSpace *mount_space = process->mountSpace;
	mount_space->openAbsolute(process, request.path(), open_flags, open_mode,
			CALLBACK_MEMBER(this, &OpenClosure::openComplete));
}

void OpenClosure::openComplete(StdSharedPtr<VfsOpenFile> file) {
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::FILE_NOT_FOUND);
		sendResponse(*pipe, response, msgRequest);
	}else{
		int fd = process->nextFd;
		assert(fd > 0);
		process->nextFd++;
		process->allOpenFiles.insert(fd, frigg::move(file));

		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] OPEN response" << frigg::EndLog();

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		response.set_fd(fd);
		sendResponse(*pipe, response, msgRequest);
	}

	suicide(*allocator);
}

// --------------------------------------------------------
// WriteClosure
// --------------------------------------------------------

struct WriteClosure : frigg::BaseClosure<WriteClosure> {
	WriteClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void writeComplete();

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

WriteClosure::WriteClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void WriteClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	(*file)->write(request.buffer().data(), request.buffer().size(),
			CALLBACK_MEMBER(this, &WriteClosure::writeComplete));
}

void WriteClosure::writeComplete() {
	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	sendResponse(*pipe, response, msgRequest);
	suicide(*allocator);
}

// --------------------------------------------------------
// ReadClosure
// --------------------------------------------------------

struct ReadClosure : frigg::BaseClosure<ReadClosure> {
	ReadClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void readComplete(VfsError error, size_t actual_size);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
	
	frigg::String<Allocator> buffer;
};

ReadClosure::ReadClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request), buffer(*allocator) { }

void ReadClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	buffer.resize(request.size());
	(*file)->read(buffer.data(), request.size(),
			CALLBACK_MEMBER(this, &ReadClosure::readComplete));
}

void ReadClosure::readComplete(VfsError error, size_t actual_size) {
	if(error == kVfsEndOfFile) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::END_OF_FILE);
		sendResponse(*pipe, response, msgRequest);
	}else{
		assert(error == kVfsSuccess);
		// TODO: make request.size() unsigned
		frigg::String<Allocator> actual_buffer(*allocator,
				frigg::StringView(buffer).subString(0, actual_size));
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msgRequest);

		pipe->sendStringResp(actual_buffer.data(), actual_buffer.size(), msgRequest, 1);
	}

	suicide(*allocator);
}

// --------------------------------------------------------
// SeekClosure
// --------------------------------------------------------

struct SeekClosure : frigg::BaseClosure<SeekClosure> {
	SeekClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void seekComplete(uint64_t offset);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

SeekClosure::SeekClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void SeekClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::SEEK_ABS) {
		(*file)->seek(request.rel_offset(), kSeekAbs,
				CALLBACK_MEMBER(this, &SeekClosure::seekComplete));
	}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_REL) {
		(*file)->seek(request.rel_offset(), kSeekRel,
				CALLBACK_MEMBER(this, &SeekClosure::seekComplete));
	}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
		(*file)->seek(request.rel_offset(), kSeekEof,
				CALLBACK_MEMBER(this, &SeekClosure::seekComplete));
	}else{
		frigg::panicLogger.log() << "Illegal SEEK request" << frigg::EndLog();
	}
}

void SeekClosure::seekComplete(uint64_t offset) {
	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	response.set_offset(offset);
	sendResponse(*pipe, response, msgRequest);
	suicide(*allocator);
}

// --------------------------------------------------------
// MapClosure
// --------------------------------------------------------

struct MapClosure : frigg::BaseClosure<MapClosure> {
	MapClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void mmapComplete(HelHandle handle);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
	
	frigg::String<Allocator> buffer;
};

MapClosure::MapClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request), buffer(*allocator) { }

void MapClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	buffer.resize(request.size());
	(*file)->mmap(CALLBACK_MEMBER(this, &MapClosure::mmapComplete));
}

void MapClosure::mmapComplete(HelHandle handle) {
	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	
	sendResponse(*pipe, response, msgRequest);
	pipe->sendDescriptorResp(handle, msgRequest, 1);
	HEL_CHECK(helCloseDescriptor(handle));

	suicide(*allocator);
}

// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
	RequestClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process, int iteration)
	: pipe(frigg::move(pipe)), process(process), iteration(iteration) { }
	
	void operator() ();
	
private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	uint8_t buffer[1024];
	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	int iteration;
};

void RequestClosure::processRequest(managarm::posix::ClientRequest<Allocator> request,
		int64_t msg_request) {
	// check the iteration number to prevent this process from being hijacked
	if(process && iteration != process->iteration) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::DEAD_FORK);
		sendResponse(*pipe, response, msg_request);
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::INIT) {
		assert(!process);

		process = Process::init();
		initMountSpace = process->mountSpace;

		auto device = frigg::makeShared<KernelOutDevice>(*allocator);

		unsigned int major, minor;
		DeviceAllocator &char_devices = process->mountSpace->charDevices;
		char_devices.allocateDevice("misc",
				frigg::staticPtrCast<Device>(frigg::move(device)), major, minor);
	
		auto initrd_fs = frigg::construct<extern_fs::MountPoint>(*allocator,
				frigg::move(initrdPipe));
		auto initrd_path = frigg::String<Allocator>(*allocator, "/initrd");
		initMountSpace->allMounts.insert(initrd_path, initrd_fs);

		auto dev_fs = frigg::construct<dev_fs::MountPoint>(*allocator);
		auto inode = frigg::makeShared<dev_fs::CharDeviceNode>(*allocator, major, minor);
		dev_fs->getRootDirectory()->entries.insert(frigg::String<Allocator>(*allocator, "helout"),
				frigg::staticPtrCast<dev_fs::Inode>(frigg::move(inode)));
		auto dev_root = frigg::String<Allocator>(*allocator, "/dev");
		process->mountSpace->allMounts.insert(dev_root, dev_fs);

		auto pts_fs = frigg::construct<pts_fs::MountPoint>(*allocator);
		auto pts_root = frigg::String<Allocator>(*allocator, "/dev/pts");
		process->mountSpace->allMounts.insert(pts_root, pts_fs);
		
		auto sysfile_fs = frigg::construct<sysfile_fs::MountPoint>(*allocator);
		auto sysfile_root = frigg::String<Allocator>(*allocator, "/dev/sysfile");
		process->mountSpace->allMounts.insert(sysfile_root, sysfile_fs);

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::FORK) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] FORK" << frigg::EndLog();

		StdSharedPtr<Process> new_process = process->fork();

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = request.child_ip();
		state.rsp = request.child_sp();
		
		helx::Directory directory = Process::runServer(new_process);

		HelHandle thread;
		HEL_CHECK(helCreateThread(new_process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse | kHelThreadNewGroup, &thread));
		HEL_CHECK(helCloseDescriptor(thread));

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::EXEC) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] EXEC" << frigg::EndLog();

		execute(process, request.path());
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::FSTAT) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] FSTAT" << frigg::EndLog();

		frigg::runClosure<StatClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] OPEN" << frigg::EndLog();

		frigg::runClosure<OpenClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] WRITE" << frigg::EndLog();

		frigg::runClosure<WriteClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::READ) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] READ" << frigg::EndLog();

		frigg::runClosure<ReadClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_ABS
			|| request.request_type() == managarm::posix::ClientRequestType::SEEK_REL
			|| request.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] SEEK" << frigg::EndLog();

		frigg::runClosure<SeekClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::MMAP) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] MMAP" << frigg::EndLog();

		frigg::runClosure<MapClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::CLOSE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] CLOSE" << frigg::EndLog();

		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t fd = request.fd();
		auto file_wrapper = process->allOpenFiles.get(fd);
		if(file_wrapper){
			process->allOpenFiles.remove(fd);
			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}
		
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::DUP2) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] DUP2" << frigg::EndLog();

		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t oldfd = request.fd();
		int32_t newfd = request.newfd();
		auto file_wrapper = process->allOpenFiles.get(oldfd);
		if(file_wrapper){
			auto file = *file_wrapper;
			process->allOpenFiles.insert(newfd, file);

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_ATTACH) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] HELFD_ATTACH" << frigg::EndLog();

		HelError error;
		HelHandle handle;
		//FIXME
		pipe->recvDescriptorReqSync(eventHub, msg_request, 1, error, handle);
		HEL_CHECK(error);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(*pipe, response, msg_request);
			return;
		}

		auto file = *file_wrapper;
		file->setHelfd(handle);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_CLONE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] HELFD_CLONE" << frigg::EndLog();

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(*pipe, response, msg_request);
			return;
		}

		auto file = *file_wrapper;
		pipe->sendDescriptorResp(file->getHelfd(), msg_request, 1);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else{
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
		sendResponse(*pipe, response, msg_request);
	}
}

void RequestClosure::operator() () {
	HelError error = pipe->recvStringReq(buffer, 1024, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &RequestClosure::recvRequest));
	if(error == kHelErrPipeClosed) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
}

void RequestClosure::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	if(error == kHelErrPipeClosed) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);

	managarm::posix::ClientRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);
	processRequest(frigg::move(request), msg_request);

	(*this)();
}

// --------------------------------------------------------
// AcceptClosure
// --------------------------------------------------------

struct AcceptClosure : frigg::BaseClosure<AcceptClosure> {
public:
	AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration);

	void operator() ();

private:
	void accepted(HelError error, HelHandle handle);

	helx::Server p_server;
	frigg::SharedPtr<Process> process;
	int iteration;
};

AcceptClosure::AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration)
: p_server(frigg::move(server)), process(frigg::move(process)), iteration(iteration) { }

void AcceptClosure::operator() () {
	p_server.accept(eventHub, CALLBACK_MEMBER(this, &AcceptClosure::accepted));
}

void AcceptClosure::accepted(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	
	auto pipe = frigg::makeShared<helx::Pipe>(*allocator, handle);
	frigg::runClosure<RequestClosure>(*allocator, frigg::move(pipe), process, iteration);
	(*this)();
}

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration) {
	frigg::runClosure<AcceptClosure>(*allocator, frigg::move(server),
			frigg::move(process), iteration);
}

// --------------------------------------------------------
// QueryDeviceIfClosure
// --------------------------------------------------------

struct QueryDeviceIfClosure {
	QueryDeviceIfClosure(int64_t request_id);

	void operator() ();

private:
	void recvdPipe(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle);

	int64_t requestId;
};

QueryDeviceIfClosure::QueryDeviceIfClosure(int64_t request_id)
: requestId(request_id) { }

void QueryDeviceIfClosure::operator() () {
	mbusPipe.recvDescriptorResp(eventHub, requestId, 1,
			CALLBACK_MEMBER(this, &QueryDeviceIfClosure::recvdPipe));
}

void QueryDeviceIfClosure::recvdPipe(HelError error, int64_t msg_request, int64_t msq_seq,
		HelHandle handle) {
	auto fs = frigg::construct<extern_fs::MountPoint>(*allocator, helx::Pipe(handle));
	auto path = frigg::String<Allocator>(*allocator, "");
	initMountSpace->allMounts.insert(path, fs);

}

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure : public frigg::BaseClosure<MbusClosure> {
	void operator() ();

private:
	void recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	HEL_CHECK(mbusPipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &MbusClosure::recvdBroadcast)));
}

bool hasCapability(const managarm::mbus::SvrRequest<Allocator> &svr_request,
		frigg::StringView name) {
	for(size_t i = 0; i < svr_request.caps_size(); i++)
		if(svr_request.caps(i).name() == name)
			return true;
	return false;
}

void MbusClosure::recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest<Allocator> svr_request(*allocator);
	svr_request.ParseFromArray(buffer, length);

	if(hasCapability(svr_request, "file-system")) {
		managarm::mbus::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
		request.set_object_id(svr_request.object_id());

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);

		frigg::runClosure<QueryDeviceIfClosure>(*allocator, 1);
	}

	(*this)();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::EndLog();
	allocator.initialize(virtualAlloc);

	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();

	// connect to mbus
	const char *mbus_path = "local/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	mbusConnect = helx::Client(mbus_handle);
	
	HelError mbus_connect_error;
	mbusConnect.connectSync(eventHub, mbus_connect_error, mbusPipe);
	HEL_CHECK(mbus_connect_error);

	// enumerate the initrd object
	managarm::mbus::CntRequest<Allocator> enum_request(*allocator);
	enum_request.set_req_type(managarm::mbus::CntReqType::ENUMERATE);
	
	managarm::mbus::Capability<Allocator> cap(*allocator);
	cap.set_name(frigg::String<Allocator>(*allocator, "initrd"));
	enum_request.add_caps(frigg::move(cap));

	frigg::String<Allocator> enum_serialized(*allocator);
	enum_request.SerializeToString(&enum_serialized);
	mbusPipe.sendStringReq(enum_serialized.data(), enum_serialized.size(), 0, 0);

	uint8_t enum_buffer[128];
	HelError enum_error;
	size_t enum_length;
	mbusPipe.recvStringRespSync(enum_buffer, 128, eventHub, 0, 0, enum_error, enum_length);
	HEL_CHECK(enum_error);
	
	managarm::mbus::SvrResponse<Allocator> enum_response(*allocator);
	enum_response.ParseFromArray(enum_buffer, enum_length);
	
	// query the initrd object
	managarm::mbus::CntRequest<Allocator> query_request(*allocator);
	query_request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
	query_request.set_object_id(enum_response.object_id());

	frigg::String<Allocator> query_serialized(*allocator);
	query_request.SerializeToString(&query_serialized);
	mbusPipe.sendStringReq(query_serialized.data(), query_serialized.size(), 0, 0);
	
	HelError query_error;
	HelHandle query_handle;
	mbusPipe.recvDescriptorRespSync(eventHub, 0, 1, query_error, query_handle);
	HEL_CHECK(query_error);
	initrdPipe = helx::Pipe(query_handle);

	frigg::runClosure<MbusClosure>(*allocator);

	// start our own server
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::move(server), StdSharedPtr<Process>(), 0);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0, kHelRequest));
	client.reset();

	while(true) {
		eventHub.defaultProcessEvents();
	}
}

asm ( ".global _start\n"
		"_start:\n"
		"\tcall main\n"
		"\tud2" );

extern "C"
int __cxa_atexit(void (*func) (void *), void *arg, void *dso_handle) {
	return 0;
}

void *__dso_handle;

