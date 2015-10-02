
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/string.hpp>
#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/async2.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/elf.hpp>
#include <frigg/protobuf.hpp>

#include "ld-server.frigg_pb.hpp"
#include "posix.frigg_pb.hpp"

void loadImage(const char *path, HelHandle directory, bool exclusive) {
	// open and map the executable image into this address space
	HelHandle image_handle;
	HEL_CHECK(helRdOpen(path, strlen(path), &image_handle));

	size_t size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, size,
			kHelMapReadOnly, &image_ptr));
	HEL_CHECK(helCloseDescriptor(image_handle));
	
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	assert(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	assert(ehdr->e_type == ET_EXEC);
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			const size_t kPageSize = 0x1000;

			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			// map the segment memory as read/write and initialize it
			HelHandle memory;
			HEL_CHECK(helAllocateMemory(virt_length, 0, &memory));
			
			void *write_ptr;
			HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, virt_length,
					kHelMapReadWrite, &write_ptr));

			memset(write_ptr, 0, virt_length);
			memcpy((void *)((uintptr_t)write_ptr + (phdr->p_vaddr - virt_address)),
					(void *)((uintptr_t)image_ptr + phdr->p_offset), phdr->p_filesz);
			HEL_CHECK(helUnmapMemory(kHelNullHandle, write_ptr, virt_length));
			
			// map the segment memory to its own address space
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				frigg::panicLogger.log() << "Illegal combination of segment permissions"
						<< frigg::EndLog();
			}

			void *actual_ptr;
			HEL_CHECK(helMapMemory(memory, space, (void *)virt_address, virt_length,
					map_flags, &actual_ptr));
			HEL_CHECK(helCloseDescriptor(memory));
		}else if(phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}
	
	constexpr size_t stack_size = 0x10000;
	
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, space, nullptr,
			stack_size, kHelMapReadWrite, &stack_base));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	HelThreadState state;
	memset(&state, 0, sizeof(HelThreadState));
	state.rip = ehdr->e_entry;
	state.rsp = (uintptr_t)stack_base + stack_size;

	HelHandle thread;
	uint32_t thread_flags = kHelThreadNewUniverse;
	if(exclusive)
		thread_flags |= kHelThreadExclusive;
	HEL_CHECK(helCreateThread(space, directory, &state, thread_flags, &thread));
	HEL_CHECK(helCloseDescriptor(space));
}

helx::EventHub eventHub = helx::EventHub::create();
helx::Client ldServerConnect;
helx::Client acpiConnect;
helx::Pipe ldServerPipe;
helx::Pipe posixPipe;

struct StartFreeContext {
	StartFreeContext()
	: directory(helx::Directory::create()), localDirectory(helx::Directory::create()) {
	}

	helx::Directory directory;
	helx::Directory localDirectory;
	helx::Pipe childPipe;
};

void startAcpi() {
	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createBiDirection(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	loadImage("initrd/acpi", directory.getHandle(), true);
	
	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	acpiConnect = helx::Client(connect_handle);
}

void startLdServer() {
	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createBiDirection(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	directory.remount("initrd/#this", "initrd");
	loadImage("initrd/ld-server", directory.getHandle(), false);
	
	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	ldServerConnect = helx::Client(connect_handle);

	HelError error;
	ldServerConnect.connectSync(eventHub, error, ldServerPipe);
	HEL_CHECK(error);
}

void startPosixSubsystem() {
	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createBiDirection(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	local_directory.publish(ldServerConnect.getHandle(), "rtdl-server");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	loadImage("initrd/posix-subsystem", directory.getHandle(), false);
	
	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	
	helx::Client posix_connect(connect_handle);
	HelError connect_error;
	posix_connect.connectSync(eventHub, connect_error, posixPipe);
	HEL_CHECK(connect_error);
}

void posixDoRequest(managarm::posix::ClientRequest<Allocator> &request,
		managarm::posix::ServerResponse<Allocator> &response, int64_t request_id) {

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	posixPipe.sendString(serialized.data(), serialized.size(), request_id, 0);
	
	uint8_t buffer[128];
	HelError error;
	size_t length;
	posixPipe.recvStringSync(buffer, 128, eventHub, request_id, 0, error, length);
	HEL_CHECK(error);
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
	open_request.set_path(frigg::String<Allocator>(*allocator, "/dev/hw"));
	open_request.set_flags(managarm::posix::OpenFlags::CREAT);
	open_request.set_mode(managarm::posix::OpenMode::HELFD);
	
	managarm::posix::ServerResponse<Allocator> open_response(*allocator);
	posixDoRequest(open_request, open_response, 2);
	assert(open_response.error() == managarm::posix::Errors::SUCCESS);
	
	// attach the server to the helfd
	managarm::posix::ClientRequest<Allocator> attach_request(*allocator);
	attach_request.set_request_type(managarm::posix::ClientRequestType::HELFD_ATTACH);
	attach_request.set_fd(open_response.fd());

	posixPipe.sendDescriptor(acpiConnect.getHandle(), 3, 1);
	
	managarm::posix::ServerResponse<Allocator> attach_response(*allocator);
	posixDoRequest(attach_request, attach_response, 3);
	assert(attach_response.error() == managarm::posix::Errors::SUCCESS);
	
	// after that we EXEC the actual init program
	managarm::posix::ClientRequest<Allocator> exec_request(*allocator);
	exec_request.set_request_type(managarm::posix::ClientRequestType::EXEC);
	exec_request.set_path(frigg::String<Allocator>(*allocator, "posix-init"));
	
	managarm::posix::ServerResponse<Allocator> exec_response(*allocator);
	posixDoRequest(exec_request, exec_response, 4);
	assert(exec_response.error() == managarm::posix::Errors::SUCCESS);
}

extern "C" void exit(int status) {
	HEL_CHECK(helExitThisThread());
}

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();

	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering user_boot" << frigg::EndLog();
	allocator.initialize(virtualAlloc);
	
	startAcpi();
	startLdServer();
	startPosixSubsystem();
	runPosixInit();
	
	infoLogger->log() << "user_boot completed successfully" << frigg::EndLog();
	HEL_CHECK(helExitThisThread());
	__builtin_unreachable();
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

