
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <functional>
#include <iostream>
#include <string>

#include <cofiber.hpp>
#include <frigg/elf.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

// TODO: remove/rename those functions from mlibc
HelHandle __raw_map(int fd);
int __mlibc_pushFd(HelHandle handle);

helix::UniqueLane mbusMasterLane;

// --------------------------------------------------------
// ELF parsing and loading.
// --------------------------------------------------------

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	std::string interpreter;
};

ImageInfo loadImage(HelHandle space, const char *path, uintptr_t base) {
	ImageInfo info;

	// open and map the executable image into this address space.
	int fd = open(path, O_RDONLY);
	HelHandle image_handle = __raw_map(fd);
	// TODO: close the image file.

	size_t size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, 0, size,
			kHelMapReadOnly, &image_ptr));
	HEL_CHECK(helCloseDescriptor(image_handle));
	
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	assert(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	assert(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN);

	info.entryIp = (char *)base + ehdr->e_entry;
	info.phdrEntrySize = ehdr->e_phentsize;
	info.phdrCount = ehdr->e_phnum;
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			const size_t kPageSize = 0x1000;

			// align virtual address and length to page size
			uintptr_t virt_address = base + phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (base + phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			// map the segment memory as read/write and initialize it
			HelHandle memory;
			HEL_CHECK(helAllocateMemory(virt_length, 0, &memory));
			
			void *write_ptr;
			HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, virt_length,
					kHelMapReadWrite, &write_ptr));

			memset(write_ptr, 0, virt_length);
			memcpy((char *)write_ptr + (base + phdr->p_vaddr - virt_address),
					(char *)image_ptr + phdr->p_offset, phdr->p_filesz);
			HEL_CHECK(helUnmapMemory(kHelNullHandle, write_ptr, virt_length));
			
			// map the segment memory to its own address space
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				throw std::runtime_error("Illegal combination of segment permissions");
			}

			void *actual_ptr;
			HEL_CHECK(helMapMemory(memory, space, (void *)virt_address, 0, virt_length,
					map_flags, &actual_ptr));
			HEL_CHECK(helCloseDescriptor(memory));
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_INTERP) {
			info.interpreter = std::string((char *)image_ptr + phdr->p_offset,
					phdr->p_filesz);
		}else if(phdr->p_type == PT_DYNAMIC
				|| phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			throw std::runtime_error("Unexpected PHDR");
		}
	}

	return info;
}

// --------------------------------------------------------
// Utilities
// --------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, serveStdout(helix::UniqueDescriptor s),
		[lane = std::move(s)] () {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;
		helix::RecvBuffer<M> recv_data;
		helix::SendBuffer<M> send_resp;

		char buffer[128];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 128)
		}, helix::Dispatcher::global());

		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto request_lane = accept.descriptor();

		// FIXME: actually parse the protocol.

		char data[256];
		helix::submitAsync(request_lane, {
			helix::action(&recv_data, data, 256)
		}, helix::Dispatcher::global());
		
		COFIBER_AWAIT recv_data.future();
		HEL_CHECK(recv_data.error());

		helLog(data, recv_data.actualLength());

		// send the success response.
		// FIXME: send an actually valid answer.
		helix::submitAsync(request_lane, {
			helix::action(&send_resp, nullptr, 0)
		}, helix::Dispatcher::global());
		
		COFIBER_AWAIT send_resp.future();
		HEL_CHECK(send_resp.error());
	}
})

// --------------------------------------------------------
// Process image construction.
// --------------------------------------------------------

template<typename Iterator>
size_t copyArrayToStack(void *window, size_t &d, Iterator begin, Iterator end) {
	using T = typename std::iterator_traits<Iterator>::value_type;
	static_assert(std::is_trivially_copyable<T>::value, "Need trivially copyable type");
	assert(d >= alignof(T) + sizeof(T));

	d -= d % alignof(T);
	d -= sizeof(T) * (end - begin);
	auto ptr = (char *)window + d;
	std::copy(begin, end, (T *)ptr);
	return d;
}

template<typename Range>
size_t copyArrayToStack(void *window, size_t &d, const Range &range) {
	return copyArrayToStack(window, d, std::begin(range), std::end(range));
}

void runProgram(HelHandle space, helix::UniquePipe xpipe,
		ImageInfo exec_info, ImageInfo interp_info, bool exclusive) {
	constexpr size_t stack_size = 0x10000;
	
	// TODO: we should use separate stdin/out/err pipes.
	helix::UniqueDescriptor stdout_server, stdout_client;
	std::tie(stdout_server, stdout_client) = helix::createStream();
	serveStdout(std::move(stdout_server));

	unsigned long posix_server;
	if(peekauxval(AT_POSIX_SERVER, &posix_server))
		throw std::runtime_error("No AT_POSIX_SERVER specified");

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));

	HelHandle remote_stdout;
	HelHandle remote_posix;
	HEL_CHECK(helTransferDescriptor(stdout_client.getHandle(), universe, &remote_stdout));
	HEL_CHECK(helTransferDescriptor(posix_server, universe, &remote_posix));
	
	// allocate a stack and map it into the new address space.	
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));
	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, space, nullptr,
			0, stack_size, kHelMapReadWrite, &stack_base));

	// map the stack into our address space and set it up.
	void *window;
	HEL_CHECK(helMapMemory(stack_memory, kHelNullHandle, nullptr, 0, stack_size,
			kHelMapReadWrite, &window));

	// the offset at which the stack image starts.
	size_t d = stack_size;

	struct FileEntry {
		int fd;
		HelHandle pipe;
	};

	// TODO: we should use some dup request here to avoid requestId clashes.
	size_t files_offset = copyArrayToStack(window, d, (FileEntry[]) {
		{ 0, remote_stdout },
		{ 1, remote_stdout },
		{ 2, remote_stdout },
		{ -1, kHelNullHandle }
	});

	// setup the auxiliary vector and copy it to the target stack.
	std::vector<uintptr_t> tail;
	tail.push_back(AT_ENTRY);
	tail.push_back(uintptr_t(exec_info.entryIp));
	tail.push_back(AT_PHDR);
	tail.push_back(uintptr_t(exec_info.phdrPtr));
	tail.push_back(AT_PHENT);
	tail.push_back(exec_info.phdrEntrySize);
	tail.push_back(AT_PHNUM);
	tail.push_back(exec_info.phdrCount);
	tail.push_back(AT_OPENFILES);
	tail.push_back(uintptr_t(stack_base) + files_offset);
	tail.push_back(AT_POSIX_SERVER);
	tail.push_back(uintptr_t(remote_posix));
	
	if(xpipe.getHandle()) {
		HelHandle remote;
		HEL_CHECK(helTransferDescriptor(xpipe.getHandle(), universe, &remote));
		tail.push_back(AT_XPIPE);
		tail.push_back(remote);
	}
	if(mbusMasterLane.getHandle()) {
		HelHandle remote;
		HEL_CHECK(helTransferDescriptor(mbusMasterLane.getHandle(), universe, &remote));
		tail.push_back(AT_MBUS_SERVER);
		tail.push_back(remote);
	}

	tail.push_back(AT_NULL);
	tail.push_back(0);
	copyArrayToStack(window, d, tail);
	
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, stack_size));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	// finally create a thread for the program.
	HelHandle thread;
	uint32_t thread_flags = kHelThreadTrapsAreFatal;
	if(exclusive)
		thread_flags |= kHelThreadExclusive;
	HEL_CHECK(helCreateThread(universe, space, kHelAbiSystemV,
			interp_info.entryIp, (char *)stack_base + d, thread_flags, &thread));
	HEL_CHECK(helCloseDescriptor(space));
}

// --------------------------------------------------------
// Individual services handling.
// --------------------------------------------------------

void startMbus() {
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	
	helix::UniqueLane xpipe;
	std::tie(mbusMasterLane, xpipe) = helix::createStream();

	ImageInfo exec_info = loadImage(space, "mbus", 0);
	// TODO: actually use the correct interpreter
	ImageInfo interp_info = loadImage(space, "ld-init.so", 0x40000000);
	runProgram(space, std::move(xpipe), exec_info, interp_info, true);
	
	// receive a client handle from the child process
	/*HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	mbusConnect = helx::Client(connect_handle);*/
}

void startAcpi() {
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));

	helix::UniquePipe xpipe_local, xpipe_remote;
	std::tie(xpipe_local, xpipe_remote) = helix::createFullPipe();

	ImageInfo exec_info = loadImage(space, "acpi", 0);
	// TODO: actually use the correct interpreter
	ImageInfo interp_info = loadImage(space, "ld-init.so", 0x40000000);
	runProgram(space, std::move(xpipe_remote), exec_info, interp_info, true);

/*	auto directory = helx::Directory::create();
	HelHandle universe = loadImage("initrd/acpi", directory.getHandle(), true);
	helx::Pipe pipe(universe);
	
	// TODO: use a real protocol here!
	HelError mbus_error;
	pipe.sendDescriptorRespSync(mbusConnect.getHandle(), eventHub, 1001, 0, mbus_error);
	HEL_CHECK(mbus_error);

	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	acpiConnect = helx::Client(connect_handle);*/
}

void startUhci() {
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	
	helix::UniquePipe xpipe_local, xpipe_remote;
	std::tie(xpipe_local, xpipe_remote) = helix::createFullPipe();

	ImageInfo exec_info = loadImage(space, "uhci", 0);
	// TODO: actually use the correct interpreter
	ImageInfo interp_info = loadImage(space, "ld-init.so", 0x40000000);
	runProgram(space, std::move(xpipe_remote), exec_info, interp_info, true);
}

void startPosixSubsystem() {
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	
	helix::UniquePipe xpipe_local, xpipe_remote;
	std::tie(xpipe_local, xpipe_remote) = helix::createFullPipe();

	ImageInfo exec_info = loadImage(space, "posix-subsystem", 0);
	// TODO: actually use the correct interpreter
	ImageInfo interp_info = loadImage(space, "ld-init.so", 0x40000000);
	runProgram(space, std::move(xpipe_remote), exec_info, interp_info, true);

/*	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createFullPipe(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	local_directory.publish(mbusConnect.getHandle(), "mbus");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	directory.remount("initrd/#this", "initrd");
	loadImage("initrd/posix-subsystem", directory.getHandle(), false);
	
	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	
	helx::Client posix_connect(connect_handle);
	HelError connect_error;
	posix_connect.connectSync(eventHub, connect_error, posixPipe);
	HEL_CHECK(connect_error);*/
}

/*
void posixDoRequest(managarm::posix::ClientRequest<Allocator> &request,
		managarm::posix::ServerResponse<Allocator> &response, int64_t request_id) {

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	
	HelError send_error; 
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_id, 0, send_error);
	HEL_CHECK(send_error);

	uint8_t buffer[128];
	HelError recv_error;
	size_t length;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_id, 0, recv_error, length);
	HEL_CHECK(recv_error);
	response.ParseFromArray(buffer, length);
}

void posixDoRequestWithHandle(HelHandle handle, managarm::posix::ClientRequest<Allocator> &request,
		managarm::posix::ServerResponse<Allocator> &response, int64_t request_id) {

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	
	HelError send_error; 
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_id, 0, send_error);
	HEL_CHECK(send_error);

	HelError descriptor_error;
	posixPipe.sendDescriptorReqSync(handle, eventHub, request_id, 1, descriptor_error);
	HEL_CHECK(descriptor_error);
	
	uint8_t buffer[128];
	HelError recv_error;
	size_t length;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_id, 0, recv_error, length);
	HEL_CHECK(recv_error);
	response.ParseFromArray(buffer, length);
}

void runPosixInit() {
	// first we send an INIT request to create an initial process
	managarm::posix::ClientRequest<Allocator> init_request(*allocator);
	init_request.set_request_type(managarm::posix::ClientRequestType::INIT);
	
	managarm::posix::ServerResponse<Allocator> init_response(*allocator);
	posixDoRequest(init_request, init_response, 1);
	assert(init_response.error() == managarm::posix::Errors::SUCCESS);
	
	// create a helfd file for the hardware driver
	managarm::posix::ClientRequest<Allocator> open_request(*allocator);
	open_request.set_request_type(managarm::posix::ClientRequestType::OPEN);
	open_request.set_path(frigg::String<Allocator>(*allocator, "/dev/sysfile/hw"));
	open_request.set_flags(managarm::posix::OpenFlags::CREAT);
	open_request.set_mode(managarm::posix::OpenMode::HELFD);
	
	managarm::posix::ServerResponse<Allocator> open_response(*allocator);
	posixDoRequest(open_request, open_response, 2);
	assert(open_response.error() == managarm::posix::Errors::SUCCESS);
	
	// attach the server to the helfd
	managarm::posix::ClientRequest<Allocator> attach_request(*allocator);
	attach_request.set_request_type(managarm::posix::ClientRequestType::HELFD_ATTACH);
	attach_request.set_fd(open_response.fd());

	managarm::posix::ServerResponse<Allocator> attach_response(*allocator);
	posixDoRequestWithHandle(acpiConnect.getHandle(), attach_request, attach_response, 3);
	assert(attach_response.error() == managarm::posix::Errors::SUCCESS);
	
	// after that we EXEC the actual init program
	managarm::posix::ClientRequest<Allocator> exec_request(*allocator);
	exec_request.set_request_type(managarm::posix::ClientRequestType::EXEC);
	exec_request.set_path(frigg::String<Allocator>(*allocator, "/initrd/posix-init"));
	
	managarm::posix::ServerResponse<Allocator> exec_response(*allocator);
	posixDoRequest(exec_request, exec_response, 4);
	assert(exec_response.error() == managarm::posix::Errors::SUCCESS);
}*/

extern "C" void __rtdl_setupTcb();

void serveMain() {
	// we use the raw managarm thread API so we have to setup
	// the TCB ourselfs here.
	assert(__rtdl_setupTcb);
	__rtdl_setupTcb();

	while(true)
		helix::Dispatcher::global().dispatch();
	__builtin_trap();
}

int main() {
	// we need a second thread to serve stdout.
	// this cannot be done in this thread as libc uses blocking calls.
	HelHandle thread_handle;
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle,
			kHelAbiSystemV, (void *)serveMain, (char *)malloc(0x10000) + 0x10000,
			kHelThreadExclusive, &thread_handle));
	
	helix::UniqueDescriptor server, client;
	std::tie(server, client) = helix::createStream();
	serveStdout(std::move(server));
	
	// TODO: we should use separate stdin/out/err pipes.
	__mlibc_pushFd(client.getHandle());
	__mlibc_pushFd(client.getHandle());
	__mlibc_pushFd(client.getHandle());
	client.release();

	std::cout << "Entering user_boot" << std::endl;
	
	startMbus();
//	startAcpi();
//	startUhci();
	startPosixSubsystem();
//	runPosixInit();

	std::cout << "user_boot completed successfully" << std::endl;
}

