
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/memory.hpp>
#include <frigg/debug.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/callback.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "exec.hpp"

#include "ld-server.frigg_pb.hpp"

// --------------------------------------------------------
// LoadClosure
// --------------------------------------------------------

struct LoadClosure {
	LoadClosure(HelHandle space, frigg::String<Allocator> path, uintptr_t base_address,
			frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback);

	void operator() ();

private:
	void recvdResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void processSegment();
	void recvdSegment(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle);

	HelHandle space;
	frigg::String<Allocator> path;
	uintptr_t baseAddress;
	frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback;

	uint8_t buffer[128];
	managarm::ld_server::ServerResponse<Allocator> response;
	size_t currentSegment;
};

LoadClosure::LoadClosure(HelHandle space, frigg::String<Allocator> path, uintptr_t base_address,
		frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback)
: space(space), path(frigg::move(path)), baseAddress(base_address), callback(callback),
		response(*allocator), currentSegment(0) { }

void LoadClosure::operator() () {
	managarm::ld_server::ClientRequest<Allocator> request(*allocator);
	request.set_identifier(frigg::move(path));
	request.set_base_address(baseAddress);
	
	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	ldServerPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	HEL_CHECK(ldServerPipe.recvStringResp(buffer, 128, eventHub, 1, 0,
			CALLBACK_MEMBER(this, &LoadClosure::recvdResponse)));
}

void LoadClosure::recvdResponse(HelError error,
		int64_t msg_request, int64_t msg_seq, size_t length) {
	HEL_CHECK(error);

	response.ParseFromArray(buffer, length);
	processSegment();
}

void LoadClosure::processSegment() {
	if(currentSegment < response.segments_size()) {
		ldServerPipe.recvDescriptorResp(eventHub, 1, 1 + currentSegment,
				CALLBACK_MEMBER(this, &LoadClosure::recvdSegment));
	}else{
		callback(response.entry(), response.phdr_pointer(), response.phdr_entry_size(),
				response.phdr_count());
		frigg::destruct(*allocator, this);
	}
}

void LoadClosure::recvdSegment(HelError error,
		int64_t msg_request, int64_t msg_seq, HelHandle handle) {
	HEL_CHECK(error);

	auto &segment = response.segments(currentSegment);

	uint32_t map_flags = 0;
	if(segment.access() == managarm::ld_server::Access::READ_WRITE) {
		map_flags |= kHelMapReadWrite;
	}else{
		assert(segment.access() == managarm::ld_server::Access::READ_EXECUTE);
		map_flags |= kHelMapReadExecute | kHelMapShareOnFork;
	}
	
	void *actual_ptr;
	HEL_CHECK(helMapMemory(handle, space, (void *)segment.virt_address(),
			segment.virt_length(), map_flags, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(handle));
	currentSegment++;

	processSegment();
}

// --------------------------------------------------------
// ExecuteClosure
// --------------------------------------------------------

struct ExecuteClosure {
	ExecuteClosure(frigg::SharedPtr<Process> process, frigg::String<Allocator> path);
	
	void operator() ();

private:
	void loadedExecutable(uintptr_t entry, uintptr_t phdr_pointer, size_t phdr_entry_size,
			size_t phdr_count);
	void loadedInterpreter(uintptr_t entry, uintptr_t phdr_pointer, size_t phdr_entry_size,
			size_t phdr_count);

	frigg::SharedPtr<Process> process;
	frigg::String<Allocator> path;
	uintptr_t programEntry;
	uintptr_t phdrPointer;
	size_t phdrEntrySize, phdrCount;
	uintptr_t interpreterEntry;
};

ExecuteClosure::ExecuteClosure(frigg::SharedPtr<Process> process, frigg::String<Allocator> path)
: process(frigg::move(process)), path(frigg::move(path)) { }

void ExecuteClosure::operator() () {
	// reset the virtual memory space of the process
	if(process->vmSpace != kHelNullHandle)
		HEL_CHECK(helCloseDescriptor(process->vmSpace));
	HEL_CHECK(helCreateSpace(&process->vmSpace));
	process->iteration++;

	frigg::runClosure<LoadClosure>(*allocator, process->vmSpace, path, 0,
			CALLBACK_MEMBER(this, &ExecuteClosure::loadedExecutable));
}

void ExecuteClosure::loadedExecutable(uintptr_t entry, uintptr_t phdr_pointer,
		size_t phdr_entry_size, size_t phdr_count) {
	programEntry = entry;
	phdrPointer = phdr_pointer;
	phdrEntrySize = phdr_entry_size;
	phdrCount = phdr_count;

	frigg::runClosure<LoadClosure>(*allocator, process->vmSpace,
			frigg::String<Allocator>(*allocator, "ld-init.so"), 0x40000000,
			CALLBACK_MEMBER(this, &ExecuteClosure::loadedInterpreter));
}

void ExecuteClosure::loadedInterpreter(uintptr_t entry, uintptr_t phdr_pointer,
		size_t phdr_entry_size, size_t phdr_count) {
	interpreterEntry = entry;

	constexpr size_t stack_size = 0x10000;
	
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, process->vmSpace, nullptr,
			stack_size, kHelMapReadWrite, &stack_base));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	HelThreadState state;
	memset(&state, 0, sizeof(HelThreadState));
	state.rip = interpreterEntry;
	state.rsp = (uintptr_t)stack_base + stack_size;
	state.rdi = phdrPointer;
	state.rsi = phdrEntrySize;
	state.rdx = phdrCount;
	state.rcx = programEntry;
	
	helx::Directory directory = Process::runServer(process);

	HelHandle thread;
	HEL_CHECK(helCreateThread(process->vmSpace, directory.getHandle(),
			&state, kHelThreadNewUniverse | kHelThreadNewGroup, &thread));
	HEL_CHECK(helCloseDescriptor(thread));
}

void execute(frigg::SharedPtr<Process> process, frigg::String<Allocator> path) {
	frigg::runClosure<ExecuteClosure>(*allocator, frigg::move(process), frigg::move(path));
}

