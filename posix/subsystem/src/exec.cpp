
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
#include <frigg/elf.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "exec.hpp"
#include "vfs.hpp"

#include "ld-server.frigg_pb.hpp"

constexpr size_t kPageSize = 0x1000;

// --------------------------------------------------------
// LoadClosure
// --------------------------------------------------------

struct LoadClosure {
	LoadClosure(frigg::SharedPtr<Process> process,
			frigg::String<Allocator> path, uintptr_t base_address,
			frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback);

	void operator() ();

private:
	void openedFile(frigg::SharedPtr<VfsOpenFile> open_file);
	void readEhdr(VfsError error, size_t length);
	void seekPhdrs();
	void readPhdrs(VfsError error, size_t length);

	void processPhdr();
	void seekSegment();
	void readSegment(VfsError error, size_t length);

	frigg::SharedPtr<Process> process;
	frigg::String<Allocator> path;
	uintptr_t baseAddress;
	frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback;

	frigg::SharedPtr<VfsOpenFile> openFile;
	Elf64_Ehdr ehdr;
	char *phdrBuffer;
	size_t currentPhdr;
	void *segmentWindow;

	uintptr_t phdrPointer;
};

LoadClosure::LoadClosure(frigg::SharedPtr<Process> process,
		frigg::String<Allocator> path, uintptr_t base_address,
		frigg::CallbackPtr<void(uintptr_t, uintptr_t, size_t, size_t)> callback)
: process(process), path(frigg::move(path)), baseAddress(base_address), callback(callback),
		currentPhdr(0) { }

void LoadClosure::operator() () {
	process->mountSpace->openAbsolute(process, path, 0, 0,
			CALLBACK_MEMBER(this, &LoadClosure::openedFile));
}

void LoadClosure::openedFile(frigg::SharedPtr<VfsOpenFile> open_file) {
	assert(open_file);
	openFile = frigg::move(open_file);
	openFile->read(&ehdr, sizeof(Elf64_Ehdr),
			CALLBACK_MEMBER(this, &LoadClosure::readEhdr));
}

void LoadClosure::readEhdr(VfsError error, size_t length) {
	assert(error == kVfsSuccess);
	assert(length == sizeof(Elf64_Ehdr));
	
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');
	assert(ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

	// read the elf program headers
	phdrBuffer = (char *)allocator->allocate(ehdr.e_phnum * ehdr.e_phentsize);
	openFile->seek(ehdr.e_phoff, CALLBACK_MEMBER(this, &LoadClosure::seekPhdrs));
}

void LoadClosure::seekPhdrs() {
	openFile->read(phdrBuffer, ehdr.e_phnum * ehdr.e_phentsize,
			CALLBACK_MEMBER(this, &LoadClosure::readPhdrs));
}

void LoadClosure::readPhdrs(VfsError error, size_t length) {
	assert(error == kVfsSuccess);
	assert(length == size_t(ehdr.e_phnum * ehdr.e_phentsize));

	processPhdr();
}

void LoadClosure::processPhdr() {
	if(currentPhdr >= ehdr.e_phnum) {
		callback(baseAddress + ehdr.e_entry, phdrPointer, ehdr.e_phentsize, ehdr.e_phnum);
		return;
	}

	auto phdr = (Elf64_Phdr *)(phdrBuffer + currentPhdr * ehdr.e_phentsize);
	
	if(phdr->p_type == PT_LOAD) {
		assert(phdr->p_memsz > 0);
		
		// align segment address and length to page size
		assert(baseAddress % kPageSize == 0);
		size_t misalign = phdr->p_vaddr % kPageSize;

		size_t map_length = phdr->p_memsz + misalign;
		if((map_length % kPageSize) != 0)
			map_length += kPageSize - (map_length % kPageSize);
		
		uintptr_t map_address = baseAddress + phdr->p_vaddr - misalign;
		
		// map the segment with write permission into this address space
		HelHandle memory;
		HEL_CHECK(helAllocateMemory(map_length, 0, &memory));

		HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
				map_length, kHelMapReadWrite, &segmentWindow));
		
		// map the segment with correct permissions into the process
		if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			void *map_pointer;
			HEL_CHECK(helMapMemory(memory, process->vmSpace, (void *)map_address,
					map_length, kHelMapReadWrite, &map_pointer));
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			void *map_pointer;
			HEL_CHECK(helMapMemory(memory, process->vmSpace, (void *)map_address,
					map_length, kHelMapReadExecute, &map_pointer));
		}else{
			frigg::panicLogger.log() << "Illegal combination of segment permissions"
					<< frigg::EndLog();
		}
		
		// read the segment contents from the file
		memset(segmentWindow, 0, map_length);
		openFile->seek(phdr->p_offset, CALLBACK_MEMBER(this, &LoadClosure::seekSegment));
	}else if(phdr->p_type == PT_PHDR) {
		phdrPointer = baseAddress + phdr->p_vaddr;
		
		currentPhdr++;
		processPhdr();
	}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_INTERP
			|| phdr->p_type == PT_TLS
			|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK) {
		currentPhdr++;
		processPhdr();
	}else{
		assert(!"Unexpected PHDR type");
	}
}

void LoadClosure::seekSegment() {
	auto phdr = (Elf64_Phdr *)(phdrBuffer + currentPhdr * ehdr.e_phentsize);
	
	size_t misalign = phdr->p_vaddr % kPageSize;
	openFile->read((char *)segmentWindow + misalign, phdr->p_filesz,
			CALLBACK_MEMBER(this, &LoadClosure::readSegment));
}

void LoadClosure::readSegment(VfsError error, size_t length) {
	auto phdr = (Elf64_Phdr *)(phdrBuffer + currentPhdr * ehdr.e_phentsize);

	assert(error == kVfsSuccess);
	assert(length == phdr->p_filesz);
	
	// unmap the segment from this address space
	size_t misalign = phdr->p_vaddr % kPageSize;
	size_t map_length = phdr->p_memsz + misalign;
	if((map_length % kPageSize) != 0)
		map_length += kPageSize - (map_length % kPageSize);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, segmentWindow, map_length));

	currentPhdr++;
	processPhdr();
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

	frigg::runClosure<LoadClosure>(*allocator, process, path, 0,
			CALLBACK_MEMBER(this, &ExecuteClosure::loadedExecutable));
}

void ExecuteClosure::loadedExecutable(uintptr_t entry, uintptr_t phdr_pointer,
		size_t phdr_entry_size, size_t phdr_count) {
	assert(entry && phdr_pointer);

	programEntry = entry;
	phdrPointer = phdr_pointer;
	phdrEntrySize = phdr_entry_size;
	phdrCount = phdr_count;

	frigg::runClosure<LoadClosure>(*allocator, process,
			frigg::String<Allocator>(*allocator, "/initrd/ld-init.so"), 0x40000000,
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

