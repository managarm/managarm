
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
#include <frigg/async2.hpp>
#include <frigg/protobuf.hpp>

#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "ld-server.frigg_pb.hpp"
#include "posix.frigg_pb.hpp"

template<typename T>
using StdSharedPtr = frigg::SharedPtr<T, Allocator>;

template<typename T>
using StdUnsafePtr = frigg::UnsafePtr<T, Allocator>;

helx::EventHub eventHub;
helx::Client ldServerConnect;
helx::Pipe ldServerPipe;

struct Process;

struct Device {
	virtual void write(const void *buffer, size_t length);
	virtual void read(void *buffer, size_t max_length, size_t &actual_length);
};

void Device::write(const void *buffer, size_t length) {
	assert(!"Illegal operation for this device");
}
void Device::read(void *buffer, size_t max_length, size_t &actual_length) {
	assert(!"Illegal operation for this device");
}

struct KernelOutDevice : public Device {
	// inherited from Device
	void write(const void *buffer, size_t length) override;
};

void KernelOutDevice::write(const void *buffer, size_t length) {
	HEL_CHECK(helLog((const char *)buffer, length));
}

struct DeviceAllocator {
private:
	struct SecondaryTable {
		SecondaryTable(frigg::String<Allocator> group_name)
		: groupName(frigg::traits::move(group_name)), minorTable(*allocator) { }

		frigg::String<Allocator> groupName;
		frigg::Vector<StdSharedPtr<Device>, Allocator> minorTable;
	};

public:
	unsigned int allocateSlot(unsigned int major, StdSharedPtr<Device> device) {
		unsigned int index = majorTable[major].minorTable.size();
		majorTable[major].minorTable.push(frigg::traits::move(device));
		return index;
	}
	
	unsigned int accessGroup(frigg::StringView group_name) {
		size_t index;
		for(index = 0; index < majorTable.size(); index++)
			if(majorTable[index].groupName == group_name)
				return index;

		majorTable.push(SecondaryTable(frigg::String<Allocator>(*allocator, group_name)));
		return index;
	}

	void allocateDevice(frigg::StringView group_name,
			StdSharedPtr<Device> device, unsigned int &major, unsigned int &minor) {
		major = accessGroup(group_name);
		minor = allocateSlot(major, frigg::traits::move(device));
	}

	StdUnsafePtr<Device> getDevice(unsigned int major, unsigned int minor) {
		if(major >= majorTable.size())
			return StdUnsafePtr<Device>();
		if(minor >= majorTable[major].minorTable.size())
			return StdUnsafePtr<Device>();
		return majorTable[major].minorTable[minor];
	}

	DeviceAllocator()
	: majorTable(*allocator) { }

private:
	frigg::Vector<SecondaryTable, Allocator> majorTable;
};

struct VfsOpenFile {
	virtual StdSharedPtr<VfsOpenFile> openAt(frigg::StringView path);
	
	virtual void write(const void *buffer, size_t length);
	virtual void read(void *buffer, size_t max_length, size_t &actual_length);

	virtual void setHelfd(HelHandle handle);
	virtual HelHandle getHelfd();
};

StdSharedPtr<VfsOpenFile> VfsOpenFile::openAt(frigg::StringView path) {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}
void VfsOpenFile::write(const void *buffer, size_t length) {
	assert(!"Illegal operation for this file");
}
void VfsOpenFile::read(void *buffer, size_t max_length, size_t &actual_length) {
	assert(!"Illegal operation for this file");
}

void VfsOpenFile::setHelfd(HelHandle handle) {
	assert(!"Illegal operation for this file");
}
HelHandle VfsOpenFile::getHelfd() {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}

struct VfsMountPoint {
	virtual StdSharedPtr<VfsOpenFile> openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode) = 0;
};

struct MountSpace {
	enum OpenFlags : uint32_t {
		kOpenCreat = 1
	};

	enum OpenMode : uint32_t {
		kOpenHelfd = 1
	};

	MountSpace();

	StdSharedPtr<VfsOpenFile> openAbsolute(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode);

	frigg::Hashmap<frigg::String<Allocator>, VfsMountPoint *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> allMounts;

	DeviceAllocator charDevices;
	DeviceAllocator blockDevices;
};

MountSpace::MountSpace()
: allMounts(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

StdSharedPtr<VfsOpenFile> MountSpace::openAbsolute(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	assert(path.size() > 0);
	assert(path[0] == '/');
	
	// splits the path into a prefix that identifies the mount point
	// and a suffix that specifies remaining path relative to this mount point
	frigg::StringView prefix = path;
	frigg::StringView suffix;
	
	while(true) {
		auto mount = allMounts.get(prefix);
		if(mount)
			return (**mount)->openMounted(process, suffix, flags, mode);

		if(prefix == "/")
			return StdSharedPtr<VfsOpenFile>();

		size_t seperator = prefix.findLast('/');
		assert(seperator != size_t(-1));
		prefix = path.subString(0, seperator);
		suffix = path.subString(seperator + 1, path.size() - (seperator + 1));
	}
};

struct Process {
	// creates a new process to run the "init" program
	static StdSharedPtr<Process> init();
	
	static helx::Directory runServer(StdSharedPtr<Process> process);

	Process();

	// creates a new process by forking an old one
	StdSharedPtr<Process> fork();
	
	// incremented when this process calls execve()
	// ensures that we don't accept new requests from old pipes after execve()
	int iteration;

	// mount namespace and virtual memory space of this process
	MountSpace *mountSpace;
	HelHandle vmSpace;

	frigg::Hashmap<int, StdSharedPtr<VfsOpenFile>,
			frigg::DefaultHasher<int>, Allocator> allOpenFiles;
	int nextFd;
};

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration);

StdSharedPtr<Process> Process::init() {
	auto new_process = frigg::makeShared<Process>(*allocator);
	new_process->mountSpace = frigg::construct<MountSpace>(*allocator);
	new_process->nextFd = 3; // reserve space for stdio

	return new_process;
}

helx::Directory Process::runServer(StdSharedPtr<Process> process) {
	int iteration = process->iteration;

	auto directory = helx::Directory::create();
	auto localDirectory = helx::Directory::create();
	auto configDirectory = helx::Directory::create();
	
	directory.mount(configDirectory.getHandle(), "config");
	directory.mount(localDirectory.getHandle(), "local");

	configDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");
	
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::traits::move(server), frigg::traits::move(process), iteration);
	localDirectory.publish(client.getHandle(), "posix");

	return directory;
}

Process::Process()
: iteration(0), mountSpace(nullptr), vmSpace(kHelNullHandle),
		allOpenFiles(frigg::DefaultHasher<int>(), *allocator), nextFd(-1) { }

StdSharedPtr<Process> Process::fork() {
	auto new_process = frigg::makeShared<Process>(*allocator);
	
	new_process->mountSpace = mountSpace;
	HEL_CHECK(helForkSpace(vmSpace, &new_process->vmSpace));

	for(auto it = allOpenFiles.iterator(); it; ++it)
		new_process->allOpenFiles.insert(it->get<0>(), it->get<1>());
	new_process->nextFd = nextFd;

	return new_process;
}

struct LoadContext {
	LoadContext()
	: space(kHelNullHandle), response(*allocator), currentSegment(0) { }
	
	HelHandle space;
	uint8_t buffer[128];
	managarm::ld_server::ServerResponse<Allocator> response;
	size_t currentSegment;
};

auto loadObject =
frigg::asyncSeq(
	frigg::wrapFuncPtr<helx::RecvStringFunction>([](LoadContext *context,
			void *cb_object, auto cb_function,
			frigg::StringView object_name, uintptr_t base_address) {
		managarm::ld_server::ClientRequest<Allocator> request(*allocator);
		request.set_identifier(frigg::String<Allocator>(*allocator, object_name));
		request.set_base_address(base_address);
		
		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		ldServerPipe.sendString(serialized.data(), serialized.size(), 1, 0);
		
		ldServerPipe.recvString(context->buffer, 128, eventHub,
				1, 0, cb_object, cb_function);
	}),
	frigg::wrapFunctor([](LoadContext *context, auto &callback, HelError error,
			int64_t msg_request, int64_t msg_seq, size_t length) {
		HEL_CHECK(error);

		context->response.ParseFromArray(context->buffer, length);
		callback();
	}),
	frigg::asyncRepeatWhile(
		frigg::wrapFunctor([] (LoadContext *context, auto &callback) {
			callback(context->currentSegment < context->response.segments_size());
		}),
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::RecvDescriptorFunction>([](LoadContext *context,
					void *cb_object, auto cb_function) {
				ldServerPipe.recvDescriptor(eventHub, 1, 1 + context->currentSegment,
						cb_object, cb_function);
			}),
			frigg::wrapFunctor([](LoadContext *context, auto &callback, HelError error,
					int64_t msg_request, int64_t msg_seq, HelHandle handle) {
				HEL_CHECK(error);

				auto &segment = context->response.segments(context->currentSegment);

				uint32_t map_flags = 0;
				if(segment.access() == managarm::ld_server::Access::READ_WRITE) {
					map_flags |= kHelMapReadWrite;
				}else{
					assert(segment.access() == managarm::ld_server::Access::READ_EXECUTE);
					map_flags |= kHelMapReadExecute | kHelMapShareOnFork;
				}
				
				void *actual_ptr;
				HEL_CHECK(helMapMemory(handle, context->space, (void *)segment.virt_address(),
						segment.virt_length(), map_flags, &actual_ptr));
				HEL_CHECK(helCloseDescriptor(handle));
				context->currentSegment++;
				callback();
			})
		)
	)
);

struct ExecuteContext {
	ExecuteContext(frigg::String<Allocator> program, StdSharedPtr<Process> process)
	: program(frigg::traits::move(program)), process(process) {
		// reset the virtual memory space of the process
		if(process->vmSpace != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(process->vmSpace));
		HEL_CHECK(helCreateSpace(&process->vmSpace));
		executableContext.space = process->vmSpace;
		interpreterContext.space = process->vmSpace;
		process->iteration++;
	}

	frigg::String<Allocator> program;
	StdSharedPtr<Process> process;

	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto executeProgram =
frigg::asyncSeq(
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(frigg::StringView(context->program.data(), context->program.size()), 0);
	}),
	frigg::subContext(&ExecuteContext::executableContext,
		frigg::wrapFunctor([](LoadContext *context, auto &callback,
				frigg::StringView object_name, uintptr_t base_address) {
			callback(object_name, base_address);
		})
	),
	frigg::subContext(&ExecuteContext::executableContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(frigg::StringView("ld-init.so"), 0x40000000);
	}),
	frigg::subContext(&ExecuteContext::interpreterContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		constexpr size_t stack_size = 0x10000;
		
		HelHandle stack_memory;
		HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

		void *stack_base;
		HEL_CHECK(helMapMemory(stack_memory, context->process->vmSpace, nullptr,
				stack_size, kHelMapReadWrite, &stack_base));
		HEL_CHECK(helCloseDescriptor(stack_memory));

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = context->interpreterContext.response.entry();
		state.rsp = (uintptr_t)stack_base + stack_size;
		state.rdi = context->executableContext.response.phdr_pointer();
		state.rsi = context->executableContext.response.phdr_entry_size();
		state.rdx = context->executableContext.response.phdr_count();
		state.rcx = context->executableContext.response.entry();

		helx::Directory directory = Process::runServer(context->process);

		HelHandle thread;
		HEL_CHECK(helCreateThread(context->process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));
		callback();
	})
);

namespace dev_fs {

struct Inode {
	virtual StdSharedPtr<VfsOpenFile> openSelf(StdUnsafePtr<Process> process) = 0;
};

class CharDeviceNode : public Inode {
public:
	class OpenFile : public VfsOpenFile {
	public:
		OpenFile(StdSharedPtr<Device> device);
		
		// inherited from VfsOpenFile
		void write(const void *buffer, size_t length) override;
		void read(void *buffer, size_t max_length, size_t &actual_length) override;
	
	private:
		StdSharedPtr<Device> p_device;
	};

	CharDeviceNode(unsigned int major, unsigned int minor);

	// inherited from Inode
	StdSharedPtr<VfsOpenFile> openSelf(StdUnsafePtr<Process> process) override;

private:
	unsigned int major, minor;
};

CharDeviceNode::CharDeviceNode(unsigned int major, unsigned int minor)
: major(major), minor(minor) { }

StdSharedPtr<VfsOpenFile> CharDeviceNode::openSelf(StdUnsafePtr<Process> process) {
	StdUnsafePtr<Device> device = process->mountSpace->charDevices.getDevice(major, minor);
	assert(device);
	auto open_file = frigg::makeShared<OpenFile>(*allocator, StdSharedPtr<Device>(device));
	return frigg::staticPointerCast<VfsOpenFile>(frigg::traits::move(open_file));
}

CharDeviceNode::OpenFile::OpenFile(StdSharedPtr<Device> device)
: p_device(frigg::traits::move(device)) { }

void CharDeviceNode::OpenFile::write(const void *buffer, size_t length) {
	p_device->write(buffer, length);
}

void CharDeviceNode::OpenFile::read(void *buffer, size_t max_length, size_t &actual_length) {
	p_device->read(buffer, max_length, actual_length);
}

class HelfdNode : public Inode {
public:
	class OpenFile : public VfsOpenFile {
	public:
		OpenFile(HelfdNode *inode);
		
		// inherited from VfsOpenFile
		void setHelfd(HelHandle handle) override;
		HelHandle getHelfd() override;
	
	private:
		HelfdNode *p_inode;
	};

	// inherited from Inode
	StdSharedPtr<VfsOpenFile> openSelf(StdUnsafePtr<Process> process) override;

private:
	HelHandle p_handle;
};

StdSharedPtr<VfsOpenFile> HelfdNode::openSelf(StdUnsafePtr<Process> process) {
	auto open_file = frigg::makeShared<OpenFile>(*allocator, this);
	return frigg::staticPointerCast<VfsOpenFile>(frigg::traits::move(open_file));
}

HelfdNode::OpenFile::OpenFile(HelfdNode *inode)
: p_inode(inode) { }

void HelfdNode::OpenFile::setHelfd(HelHandle handle) {
	p_inode->p_handle = handle;
}
HelHandle HelfdNode::OpenFile::getHelfd() {
	return p_inode->p_handle;
}

struct DirectoryNode : public Inode {
	DirectoryNode();

	StdSharedPtr<VfsOpenFile> openRelative(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode);
	
	// inherited from Inode
	StdSharedPtr<VfsOpenFile> openSelf(StdUnsafePtr<Process> process) override;

	frigg::Hashmap<frigg::String<Allocator>, StdSharedPtr<Inode>,
			frigg::DefaultHasher<frigg::StringView>, Allocator> entries;
};

DirectoryNode::DirectoryNode()
: entries(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

StdSharedPtr<VfsOpenFile> DirectoryNode::openSelf(StdUnsafePtr<Process> process) {
	assert(!"TODO: Implement this");
	__builtin_unreachable();
}

StdSharedPtr<VfsOpenFile> DirectoryNode::openRelative(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	frigg::StringView segment;
	
	size_t seperator = path.findFirst('/');
	if(seperator == size_t(-1)) {
		auto entry = entries.get(path);
		if(entry) {
			return (**entry)->openSelf(process);
		}else if((flags & MountSpace::kOpenCreat) != 0) {
			StdSharedPtr<Inode> inode;
			if((mode & MountSpace::kOpenHelfd) != 0) {
				auto real_inode = frigg::makeShared<HelfdNode>(*allocator);
				inode = frigg::staticPointerCast<Inode>(frigg::traits::move(real_inode));
			}else{
				assert(!"mode not supported");
			}
			StdSharedPtr<VfsOpenFile> open_file = inode->openSelf(process);
			entries.insert(frigg::String<Allocator>(*allocator, path), frigg::traits::move(inode));
			return open_file;
		}else{
			return StdSharedPtr<VfsOpenFile>();
		}
	}else{
		assert(!"Not tested");
		frigg::StringView segment = path.subString(0, seperator);
		frigg::StringView tail = path.subString(seperator + 1, path.size() - (seperator + 1));
		
		auto entry = entries.get(segment);
		if(!entry)
			return StdSharedPtr<VfsOpenFile>();
		auto directory = frigg::staticPointerCast<DirectoryNode>(**entry);
		return directory->openRelative(process, tail, flags, mode);
	}
}

class MountPoint : public VfsMountPoint {
public:
	MountPoint();
	
	DirectoryNode *getRootDirectory() {
		return &rootDirectory;
	}
	
	// inherited from VfsMountPoint
	StdSharedPtr<VfsOpenFile> openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode) override;
	
private:
	DirectoryNode rootDirectory;
};

MountPoint::MountPoint() { }

StdSharedPtr<VfsOpenFile> MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	return rootDirectory.openRelative(process, path, flags, mode);
}

}; // namespace dev_fs

struct RequestLoopContext {
	RequestLoopContext(helx::Pipe pipe, StdSharedPtr<Process> process, int iteration)
	: pipe(frigg::traits::move(pipe)), process(process), iteration(iteration) { }
	
	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	void sendResponse(managarm::posix::ServerResponse<Allocator> &response, int64_t msg_request) {
		frigg::String<Allocator> serialized(*allocator);
		response.SerializeToString(&serialized);
		pipe.sendString(serialized.data(), serialized.size(), msg_request, 0);
	}

	uint8_t buffer[128];
	helx::Pipe pipe;
	StdSharedPtr<Process> process;
	int iteration;
};

void RequestLoopContext::processRequest(managarm::posix::ClientRequest<Allocator> request,
		int64_t msg_request) {
	// check the iteration number to prevent this process from being hijacked
	if(process && iteration != process->iteration) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::DEAD_FORK);
		sendResponse(response, msg_request);
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::INIT) {
		assert(!process);

		process = Process::init();

		auto device = frigg::makeShared<KernelOutDevice>(*allocator);

		unsigned int major, minor;
		DeviceAllocator &char_devices = process->mountSpace->charDevices;
		char_devices.allocateDevice("misc",
				frigg::staticPointerCast<Device>(frigg::traits::move(device)), major, minor);

		auto fs = frigg::construct<dev_fs::MountPoint>(*allocator);
		auto real_inode = frigg::makeShared<dev_fs::CharDeviceNode>(*allocator, major, minor);
		fs->getRootDirectory()->entries.insert(frigg::String<Allocator>(*allocator, "helout"),
				frigg::staticPointerCast<dev_fs::Inode>(frigg::traits::move(real_inode)));

		process->mountSpace->allMounts.insert(frigg::String<Allocator>(*allocator, "/dev"), fs);

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::FORK) {
		StdSharedPtr<Process> new_process = process->fork();

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = request.child_ip();
		state.rsp = request.child_sp();
		
		helx::Directory directory = Process::runServer(new_process);

		HelHandle thread;
		HEL_CHECK(helCreateThread(new_process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::EXEC) {
		frigg::runAsync<ExecuteContext>(*allocator, executeProgram,
				request.path(), process);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
		uint32_t open_flags = 0;
		if((request.flags() & managarm::posix::OpenFlags::CREAT) != 0)
			open_flags |= MountSpace::kOpenCreat;

		uint32_t open_mode = 0;
		if((request.mode() & managarm::posix::OpenMode::HELFD) != 0)
			open_mode |= MountSpace::kOpenHelfd;

		MountSpace *mount_space = process->mountSpace;
		StdSharedPtr<VfsOpenFile> file = mount_space->openAbsolute(process, request.path(),
				open_flags, open_mode);
		assert(file);

		int fd = process->nextFd;
		assert(fd > 0);
		process->nextFd++;
		process->allOpenFiles.insert(fd, frigg::traits::move(file));

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		response.set_fd(fd);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::READ) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(file_wrapper) {
			size_t actual_len;
			auto file = **file_wrapper;
			frigg::String<Allocator> buffer(*allocator);
			buffer.resize(request.size());
			file->read(buffer.data(), request.size(), actual_len);

			request.set_buffer(frigg::String<Allocator>(*allocator,
					buffer.data(), actual_len));
			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(file_wrapper) {
			auto file = **file_wrapper;
			file->write(request.buffer().data(), request.buffer().size());

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::CLOSE) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t fd = request.fd();
		auto file_wrapper = process->allOpenFiles.get(fd);
		if(file_wrapper){
			process->allOpenFiles.remove(fd);
			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}
		
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::DUP2) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t oldfd = request.fd();
		int32_t newfd = request.newfd();
		auto file_wrapper = process->allOpenFiles.get(oldfd);
		if(file_wrapper){
			auto file = **file_wrapper;
			process->allOpenFiles.insert(newfd, file);

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_ATTACH) {
		HelHandle handle;
		pipe.recvDescriptorSync(eventHub, msg_request, 1, handle);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(response, msg_request);
			return;
		}

		auto file = **file_wrapper;
		file->setHelfd(handle);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_CLONE) {
		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(response, msg_request);
			return;
		}

		auto file = **file_wrapper;
		pipe.sendDescriptor(file->getHelfd(), msg_request, 1);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else{
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
		sendResponse(response, msg_request);
	}
}

auto requestLoop =
frigg::asyncRepeatUntil(
	frigg::asyncSeq(
		frigg::wrapFuncPtr<helx::RecvStringFunction>([] (RequestLoopContext *context,
				void *cb_object, auto cb_function) {
			context->pipe.recvString(context->buffer, 128, eventHub,
					kHelAnyRequest, 0, cb_object, cb_function);
		}),
		frigg::wrapFunctor([] (RequestLoopContext *context, auto &callback,
				HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {
			HEL_CHECK(error);

			managarm::posix::ClientRequest<Allocator> request(*allocator);
			request.ParseFromArray(context->buffer, length);
			context->processRequest(frigg::traits::move(request), msg_request);

			callback(true);
		})
	)
);

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration) {
	struct AcceptContext {
		AcceptContext(helx::Server server, StdSharedPtr<Process> process, int iteration)
		: server(frigg::traits::move(server)), process(process), iteration(iteration) { }

		helx::Server server;
		StdSharedPtr<Process> process;
		int iteration;
	};

	auto body =
	frigg::asyncRepeatUntil(
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::AcceptFunction>([] (AcceptContext *context,
					void *cb_object, auto cb_function) {
				context->server.accept(eventHub, cb_object, cb_function);
			}),
			frigg::wrapFunctor([] (AcceptContext *context, auto &callback,
					HelError error, HelHandle handle) {
				HEL_CHECK(error);
				
				frigg::runAsync<RequestLoopContext>(*allocator, requestLoop,
						helx::Pipe(handle), context->process, context->iteration);

				callback(true);
			})
		)
	);

	frigg::runAsync<AcceptContext>(*allocator, body, frigg::traits::move(server),
			process, iteration);
}

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::debug::Finish();
	allocator.initialize(virtualAlloc);

	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	const char *ld_path = "local/rtdl-server";
	HelHandle ld_handle;
	HEL_CHECK(helRdOpen(ld_path, strlen(ld_path), &ld_handle));
	ldServerConnect = ld_handle;

	int64_t ld_connect_id;
	HEL_CHECK(helSubmitConnect(ldServerConnect.getHandle(), eventHub.getHandle(),
			0, 0, &ld_connect_id));
	ldServerPipe = eventHub.waitForConnect(ld_connect_id);

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::traits::move(server), StdSharedPtr<Process>(), 0);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));

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

