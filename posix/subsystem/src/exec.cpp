#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

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

expected<ImageInfo> load(SharedFilePtr file,
		VmContext *vmContext, uintptr_t base) {
	assert(!(base & (kPageSize - 1)));
	ImageInfo info;

	// Get a handle to the file's memory.
	auto fileMemory = co_await file->accessMemory();

	// Read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	co_await file->seek(0, VfsSeek::absolute);
	co_await file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr));

	if(!(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F'))
		co_return Error::badExecutable;
	if(ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
		co_return Error::badExecutable;

	info.entryIp = (char *)base + ehdr.e_entry;
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// Read the elf program headers and load them into the address space.
	std::vector<char> phdrBuffer;
	phdrBuffer.resize(ehdr.e_phnum * ehdr.e_phentsize);
	co_await file->seek(ehdr.e_phoff, VfsSeek::absolute);
	co_await file->readExactly(nullptr, phdrBuffer.data(), ehdr.e_phnum * size_t(ehdr.e_phentsize));

	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdrBuffer.data() + i * ehdr.e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);

			size_t misalign = phdr->p_vaddr & (kPageSize - 1);
			uintptr_t mapAddress = base + phdr->p_vaddr - misalign;
			size_t mapLength = (phdr->p_memsz + misalign + kPageSize - 1) & ~(kPageSize - 1);

			// Check if we can share the segment.
			if(!(phdr->p_flags & PF_W)) {
				assert(!misalign);
				assert(!(phdr->p_offset & (kPageSize - 1)));

				// Map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), phdr->p_offset, mapLength));

					co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							phdr->p_offset, mapLength, true,
							kHelMapProtRead | kHelMapProtExecute);
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}
			}else{
				// map the segment with write permission into this address space.
				HelHandle segmentHandle;
				HEL_CHECK(helAllocateMemory(mapLength, 0, nullptr, &segmentHandle));

				void *window;
				HEL_CHECK(helMapMemory(segmentHandle, kHelNullHandle, nullptr,
						0, mapLength, kHelMapProtRead | kHelMapProtWrite, &window));

				// map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
					co_await vmContext->mapFile(mapAddress,
							helix::UniqueDescriptor{segmentHandle}, file,
							0, mapLength, true,
							kHelMapProtRead | kHelMapProtWrite);
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}

				// read the segment contents from the file.
				memset(window, 0, mapLength);
				co_await file->seek(phdr->p_offset, VfsSeek::absolute);
				co_await file->readExactly(nullptr, (char *)window + misalign, phdr->p_filesz);
				HEL_CHECK(helUnmapMemory(kHelNullHandle, window, mapLength));
			}
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_INTERP
				|| phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK
				|| phdr->p_type == PT_GNU_RELRO) {
			// ignore this PHDR here.
		}else{
			throw std::runtime_error("Unexpected PHDR type");
		}
	}

	co_return info;
}

template<typename T, size_t N>
void *copyArrayToStack(void *window, size_t &d, const T (&value)[N]) {
	assert(d >= alignof(T) + sizeof(T) * N);
	d -= sizeof(T) * N;
	d -= d & (alignof(T) - 1);
	void *ptr = (char *)window + d;
	memcpy(ptr, &value, sizeof(T) * N);
	return ptr;
}

expected<helix::UniqueDescriptor> execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vmContext, helix::BorrowedDescriptor universe,
		HelHandle mbusHandle) {
	auto execFile = co_await open(root, workdir, path);
	if(!execFile)
		co_return Error::noSuchFile;

	int nRecursions = 0;
	while(true) {
		if(nRecursions > 8) {
			std::cout << "posix: More than 8 shebang recursions" << std::endl;
			co_return Error::badExecutable;
		}

		char shebangPrefix[2];
		co_await execFile->readExactly(nullptr, shebangPrefix, 2);
		if(shebangPrefix[0] != '#' && shebangPrefix[1] != '!')
			break;

		std::string shebangStr;
		while(true) {
			if(shebangStr.size() > 128) {
				std::cout << "posix: Shebang line of excessive length" << std::endl;
				co_return Error::badExecutable;
			}

			char buffer[128];
			auto readResult = co_await execFile->readSome(nullptr, buffer, 128);
			if(auto error = std::get_if<Error>(&readResult); error) {
				std::cout << "posix: Failed to read executable" << std::endl;
				co_return Error::badExecutable;
			}
			size_t chunk = std::get<size_t>(readResult);
			if(!chunk) {
				std::cout << "posix: EOF in shebang line" << std::endl;
				co_return Error::badExecutable;
			}
			auto nlPtr = std::find(buffer, buffer + 128, '\n');
			shebangStr.insert(shebangStr.end(), buffer, nlPtr);
			if(nlPtr != buffer + 128)
				break;
		}

		// The path is the first whitespace-separated word of the line.
		// Trim spaces from the left and the right.
		auto beginPath = std::find_if_not(shebangStr.begin(), shebangStr.end(), isspace);
		auto endPath = std::find_if(beginPath, shebangStr.end(), isspace);

		// Trim space from the argument, too.
		auto beginArg = std::find_if_not(endPath, shebangStr.end(), isspace);
		auto endArg = std::find_if_not(shebangStr.rbegin(), shebangStr.rend(), isspace).base();

		// Linux looks up the interpreter in the current working directory.
		std::string interpreterPath{beginPath, endPath};
		auto interpreterFile = co_await open(root, workdir, interpreterPath);
		if(!interpreterFile)
			co_return Error::noSuchFile;

		if(!args.empty()) // Handle exec() without arguments.
			args.erase(args.begin());
		if(beginArg != endArg)
			args.insert(args.begin(), std::string{beginArg, endArg});
		args.insert(args.begin(), path);
		args.insert(args.begin(), interpreterPath);
		path = std::move(interpreterPath);
		execFile = std::move(interpreterFile);
		nRecursions++;
	}

	auto binaryFile = std::move(execFile);
	auto binaryResult = co_await load(binaryFile, vmContext.get(), 0);
	if(auto error = std::get_if<Error>(&binaryResult); error)
		co_return *error;
	auto binaryInfo = std::get<ImageInfo>(binaryResult);

	// TODO: Should we really look up the dynamic linker in the current working dir?
	auto ldsoFile = co_await open(root, workdir, "/lib/ld-init.so");
	assert(ldsoFile);
	auto ldsoResult = co_await load(ldsoFile, vmContext.get(), 0x40000000);
	if(auto error = std::get_if<Error>(&ldsoResult); error)
		co_return *error;
	auto ldsoInfo = std::get<ImageInfo>(ldsoResult);

	constexpr size_t stackSize = 0x10000;

	// Allocate memory for the stack.
	HelHandle stackHandle;
	HEL_CHECK(helAllocateMemory(stackSize, kHelAllocOnDemand, nullptr, &stackHandle));

	void *window;
	HEL_CHECK(helMapMemory(stackHandle, kHelNullHandle, nullptr,
			0, stackSize, kHelMapProtRead | kHelMapProtWrite, &window));

	// Map the stack into the new process and set it up.
	void *stackBase = co_await vmContext->mapFile(0,
			helix::UniqueDescriptor{stackHandle}, nullptr,
			0, stackSize, true, kHelMapProtRead | kHelMapProtWrite);

	// the offset at which the stack image starts.
	size_t d = stackSize;

	// Copy argument and environment strings to the stack.
	auto pushString = [&] (const std::string &str) -> uintptr_t {
		d -= str.size() + 1;
		memcpy(reinterpret_cast<char *>(window) + d, str.c_str(), str.size() + 1);
		return reinterpret_cast<uintptr_t>(stackBase) + d;
	};

	auto execfn = pushString(path);
	std::vector<uintptr_t> argsPtrs;
	for(const auto &str : args)
		argsPtrs.push_back(pushString(str));

	std::vector<uintptr_t> envPtrs;
	for(const auto &str : env)
		envPtrs.push_back(pushString(str));

	// Align the stack before pushing the args, environment and auxiliary words.
	d -= d & size_t(15);

	// Pad the stack so that it is aligned after pushing all words.
	auto pushWord = [&] (uintptr_t w) {
		assert(!(d & (alignof(uintptr_t) - 1)));
		d -= sizeof(uintptr_t);
		memcpy(reinterpret_cast<char *>(window) + d, &w, sizeof(uintptr_t));
	};

	size_t wordParity = 1 + argsPtrs.size() + 1 // Words representing argc and args.
			+ envPtrs.size() + 1; // Words representing the environment.
	if(wordParity & 1)
		pushWord(0);

	copyArrayToStack(window, d, (uintptr_t[]){
		AT_ENTRY,
		uintptr_t(binaryInfo.entryIp),
		AT_PHDR,
		uintptr_t(binaryInfo.phdrPtr),
		AT_PHENT,
		binaryInfo.phdrEntrySize,
		AT_PHNUM,
		binaryInfo.phdrCount,
		AT_MBUS_SERVER,
		static_cast<uintptr_t>(mbusHandle),
		AT_EXECFN,
		execfn,
		AT_NULL,
		0
	});

	// Push the environment pointers and arguments.
	pushWord(0); // End of environment.
	for(auto it = envPtrs.rbegin(); it != envPtrs.rend(); ++it)
		pushWord(*it);

	pushWord(0); // End of args.
	for(auto it = argsPtrs.rbegin(); it != argsPtrs.rend(); ++it)
		pushWord(*it);
	pushWord(argsPtrs.size()); // argc.

	// Stack has to be aligned at entry.
	assert(!(d & size_t(15)));

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, stackSize));

	HelHandle thread;
	HEL_CHECK(helCreateThread(universe.getHandle(),
			vmContext->getSpace().getHandle(), kHelAbiSystemV,
			(void *)ldsoInfo.entryIp, (char *)stackBase + d, 0, &thread));

	co_return helix::UniqueDescriptor{thread};
}
