
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include <cofiber.hpp>
#include <frigg/elf.hpp>

#include "common.hpp"
#include "vfs.hpp"
#include "exec.hpp"
#include <fs.pb.h>

constexpr size_t kPageSize = 0x1000;

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
};

COFIBER_ROUTINE(async::result<ImageInfo>, load(SharedFilePtr file,
		helix::BorrowedDescriptor space, uintptr_t base), ([=] {
	assert(base % kPageSize == 0);
	ImageInfo info;

	// get a handle to the file's memory.
	auto file_memory = COFIBER_AWAIT file->accessMemory();

	// read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	COFIBER_AWAIT file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr));

	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');
	assert(ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

	info.entryIp = (char *)base + ehdr.e_entry;
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// read the elf program headers and load them into the address space.
	auto phdr_buffer = (char *)malloc(ehdr.e_phnum * ehdr.e_phentsize);
	COFIBER_AWAIT file->seek(ehdr.e_phoff, VfsSeek::absolute);
	COFIBER_AWAIT file->readExactly(nullptr, phdr_buffer, ehdr.e_phnum * size_t(ehdr.e_phentsize));

	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdr_buffer + i * ehdr.e_phentsize);
		
		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);
			
			size_t misalign = phdr->p_vaddr % kPageSize;
			uintptr_t map_address = base + phdr->p_vaddr - misalign;
			size_t map_length = phdr->p_memsz + misalign;
			if((map_length % kPageSize) != 0)
				map_length += kPageSize - (map_length % kPageSize);
			
			// check if we can share the segment.
			if(!(phdr->p_flags & PF_W)) {
				assert(misalign == 0);
				assert(phdr->p_offset % kPageSize == 0);
			
				// map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(file_memory.getHandle(), phdr->p_offset, map_length));

					void *map_pointer;
					HEL_CHECK(helMapMemory(file_memory.getHandle(), space.getHandle(),
							(void *)map_address, phdr->p_offset, map_length,
							kHelMapProtRead | kHelMapProtExecute | kHelMapShareAtFork,
							&map_pointer));
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}
			}else{
				// map the segment with write permission into this address space.
				HelHandle segment_memory;
				HEL_CHECK(helAllocateMemory(map_length, 0, &segment_memory));

				void *window;
				HEL_CHECK(helMapMemory(segment_memory, kHelNullHandle, nullptr,
						0, map_length, kHelMapProtRead | kHelMapProtWrite, &window));
			
				// map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
					void *map_pointer;
					HEL_CHECK(helMapMemory(segment_memory, space.getHandle(),
							(void *)map_address, 0, map_length,
							kHelMapProtRead | kHelMapProtWrite | kHelMapCopyOnWriteAtFork,
							&map_pointer));
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}
				HEL_CHECK(helCloseDescriptor(segment_memory));

				// read the segment contents from the file.
				memset(window, 0, map_length);
				COFIBER_AWAIT file->seek(phdr->p_offset, VfsSeek::absolute);
				COFIBER_AWAIT file->readExactly(nullptr, (char *)window + misalign, phdr->p_filesz);
				HEL_CHECK(helUnmapMemory(kHelNullHandle, window, map_length));
			}
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_INTERP
				|| phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK) {
			// ignore this PHDR here.
		}else{
			throw std::runtime_error("Unexpected PHDR type");
		}
	}
	
	COFIBER_RETURN(info);
}))

template<typename T, size_t N>
void *copyArrayToStack(void *window, size_t &d, const T (&value)[N]) {
	assert(d >= alignof(T) + sizeof(T) * N);
	d -= sizeof(T) * N;
	d -= d % alignof(T);
	void *ptr = (char *)window + d;
	memcpy(ptr, &value, sizeof(T) * N);
	return ptr;
}

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, execute(ViewPath root, std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vm_context, helix::BorrowedDescriptor universe,
		HelHandle mbus_handle), ([=] {
	auto exec_file = COFIBER_AWAIT open(root, root, path);
	assert(exec_file);
	auto exec_info = COFIBER_AWAIT load(exec_file, vm_context->getSpace(), 0);
	
	auto interp_file = COFIBER_AWAIT open(root, root, "/lib/ld-init.so");
	assert(interp_file);
	auto interp_info = COFIBER_AWAIT load(interp_file, vm_context->getSpace(), 0x40000000);
	
	constexpr size_t stack_size = 0x10000;
	
	// allocate memory for the stack and map it into the remote space.
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, vm_context->getSpace().getHandle(),
			nullptr, 0, stack_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapCopyOnWriteAtFork, &stack_base));
	
	// map the stack into this process and set it up.
	void *window;
	HEL_CHECK(helMapMemory(stack_memory, kHelNullHandle, nullptr,
			0, stack_size, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	// the offset at which the stack image starts.
	size_t d = stack_size;

	// Copy argument and environment strings to the stack.
	auto pushString = [&] (const std::string &str) -> uintptr_t {
		d -= str.size() + 1;
		memcpy(reinterpret_cast<char *>(window) + d, str.c_str(), str.size() + 1);
		return reinterpret_cast<uintptr_t>(stack_base) + d;
	};

	std::vector<uintptr_t> args_ptrs;
	for(const auto &str : args)
		args_ptrs.push_back(pushString(str));

	std::vector<uintptr_t> env_ptrs;
	for(const auto &str : env)
		env_ptrs.push_back(pushString(str));

	// Align the stack before pushing the args, environment and auxiliary words.
	d -= d % 16;

	// Pad the stack so that it is aligned after pushing all words.
	auto pushWord = [&] (uintptr_t w) {
		assert(!(d % alignof(uintptr_t)));
		d -= sizeof(uintptr_t);
		memcpy(reinterpret_cast<char *>(window) + d, &w, sizeof(uintptr_t));
	};

	size_t word_parity = 1 + args_ptrs.size() + 1 // Words representing argc and args.
			+ env_ptrs.size() + 1; // Words representing the environment.
	if(word_parity % 2)
		pushWord(0);

	copyArrayToStack(window, d, (uintptr_t[]){
		AT_ENTRY,
		uintptr_t(exec_info.entryIp),
		AT_PHDR,
		uintptr_t(exec_info.phdrPtr),
		AT_PHENT,
		exec_info.phdrEntrySize,
		AT_PHNUM,
		exec_info.phdrCount,
		AT_MBUS_SERVER,
		mbus_handle,
		AT_NULL,
		0
	});
	
	// Push the environment pointers and arguments.
	pushWord(0); // End of environment.
	for(auto it = env_ptrs.rbegin(); it != env_ptrs.rend(); ++it)
		pushWord(*it);
	
	pushWord(0); // End of args.
	for(auto it = args_ptrs.rbegin(); it != args_ptrs.rend(); ++it)
		pushWord(*it);
	pushWord(args_ptrs.size()); // argc.

	// Stack has to be aligned at entry.
	assert(!(d % 16));

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, stack_size));

	HelHandle thread;
	HEL_CHECK(helCreateThread(universe.getHandle(),
			vm_context->getSpace().getHandle(), kHelAbiSystemV,
			(void *)interp_info.entryIp, (char *)stack_base + d, 0, &thread));
	
	COFIBER_RETURN(helix::UniqueDescriptor{thread});
}))

