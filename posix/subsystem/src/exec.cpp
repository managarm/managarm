#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include "vfs.hpp"
#include "exec.hpp"
#include <fs.bragi.hpp>

constexpr size_t kPageSize = 0x1000;
constexpr uintptr_t ldsoBaseAddress = 0x40000000;

// This struct is parsed before knowing the type of executable (PIE vs. non-PIE)
// and also before knowing the ELF's base address.
struct ImagePreamble {
	bool isPie = false;
};

// This struct contains the image meta data with correct base address applied.
struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	std::string interpreter;
};

async::result<frg::expected<Error, ImagePreamble>>
parseElfPreamble(SharedFilePtr file) {
	ImagePreamble preamble;

	// Read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	FRG_CO_TRY(co_await file->seek(0, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr)));

	if(!(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F'))
		co_return Error::badExecutable;
	if(ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
		co_return Error::badExecutable;

	// Right now we treat every ET_DYN object as PIE and unconditionally apply
	// a non-zero base address.
	if(ehdr.e_type == ET_DYN)
		preamble.isPie = true;

	co_return preamble;
}

async::result<frg::expected<Error, ImageInfo>>
loadElfImage(SharedFilePtr file, VmContext *vmContext, uintptr_t base) {
	assert(!(base & (kPageSize - 1))); // Callers need to ensure this.
	ImageInfo info;

	// Get a handle to the file's memory.
	auto fileMemory = co_await file->accessMemory();

	// Read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	FRG_CO_TRY(co_await file->seek(0, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr)));

	// Verify the ELF file again, since loadElfPreamble() is not necessarily called
	// on every object that we load.
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
	FRG_CO_TRY(co_await file->seek(ehdr.e_phoff, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr,
			phdrBuffer.data(), ehdr.e_phnum * size_t(ehdr.e_phentsize)));

	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdrBuffer.data() + i * ehdr.e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			if(!phdr->p_memsz) // Skip empty segments.
				continue;

			bool properlyAligned = phdr->p_offset % phdr->p_align == phdr->p_vaddr % phdr->p_align;

			size_t misalign = phdr->p_vaddr & (kPageSize - 1);
			uintptr_t mapAddress = base + phdr->p_vaddr - misalign;
			uintptr_t fileOffset = phdr->p_offset - misalign;
			size_t fileMapLength = (misalign + phdr->p_filesz + kPageSize - 1) & ~(kPageSize - 1);
			size_t totalMapLength = (misalign + phdr->p_memsz + kPageSize - 1) & ~(kPageSize - 1);

			if(!properlyAligned) {
				std::cout << "posix: ELF file with differently misaligned p_offset and p_vaddr."
						<< std::endl;
				co_return Error::badExecutable;
			}

			uint32_t nativeFlags;
			switch (phdr->p_flags & (PF_R | PF_W | PF_X)) {
				case PF_R:
					nativeFlags = kHelMapProtRead;
					break;
				case PF_X:
					[[fallthrough]];
				case PF_R | PF_X:
					nativeFlags = kHelMapProtRead | kHelMapProtExecute;
					break;
				case PF_W:
					[[fallthrough]];
				case PF_R | PF_W:
					nativeFlags = kHelMapProtRead | kHelMapProtWrite;
					break;
				default:
					// We do not support RWX.
					std::println("posix: Illegal combination of segment permissions");
					co_return Error::badExecutable;
			}

			// Map the file to up p_filesz.
			if (fileMapLength > 0) {
				HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, fileMapLength));

				auto fileArea = co_await Area::makeFile(file, fileOffset, fileMapLength, true);

				// Zero partial page from p_filesz the end of the mapping.
				size_t partialOffset = misalign + phdr->p_filesz;
				if (partialOffset < fileMapLength) {
					void *window;
					HEL_CHECK(helMapMemory(fileArea.copyView.getHandle(), kHelNullHandle, nullptr,
							0, fileMapLength, kHelMapProtRead | kHelMapProtWrite, &window));
					memset((char *)window + partialOffset, 0, fileMapLength - partialOffset);
					HEL_CHECK(helUnmapMemory(kHelNullHandle, window, fileMapLength));
				}

				FRG_CO_TRY(vmContext->mapArea(mapAddress, nativeFlags, std::move(fileArea)));
			}

			// Map anonymous memory up to p_memsz.
			if (totalMapLength > fileMapLength) {
				auto zeroArea = Area::makeAnonymous(totalMapLength - fileMapLength, true);
				FRG_CO_TRY(vmContext->mapArea(mapAddress + fileMapLength, nativeFlags, std::move(zeroArea)));
			}
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_INTERP) {
			info.interpreter.resize(phdr->p_filesz);
			FRG_CO_TRY(co_await file->seek(phdr->p_offset, VfsSeek::absolute));
			FRG_CO_TRY(co_await file->readExactly(nullptr,
					info.interpreter.data(), phdr->p_filesz));
			if(size_t n = info.interpreter.find('\0'); n != size_t(-1))
				info.interpreter.resize(n);
		}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK
				|| phdr->p_type == PT_GNU_RELRO || phdr->p_type == PT_NOTE) {
			// Ignore this PHDR here.
		}else{
			// Ignore unknown PHDRs.
			std::cout << "posix: Unexpected PHDR type " << phdr->p_type << std::endl;
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

async::result<frg::expected<Error, ExecuteResult>>
execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vmContext, helix::BorrowedDescriptor universe,
		HelHandle mbusHandle, Process *self) {
	(void) mbusHandle;

	auto execFile = FRG_CO_TRY(co_await open(root, workdir, path, self));
	assert(execFile); // If open() succeeds, it must return a non-null file.

	int nRecursions = 0;
	while(true) {
		if(nRecursions > 8) {
			std::cout << "posix: More than 8 shebang recursions" << std::endl;
			co_return Error::badExecutable;
		}

		char shebangPrefix[2];
		if(!(co_await execFile->readExactly(nullptr, shebangPrefix, 2)))
			break;
		if(shebangPrefix[0] != '#' && shebangPrefix[1] != '!')
			break;

		std::string shebangStr;
		while(true) {
			if(shebangStr.size() > 128) {
				std::cout << "posix: Shebang line of excessive length" << std::endl;
				co_return Error::badExecutable;
			}

			char buffer[128];
			auto readResult = co_await execFile->readSome(nullptr, buffer, 128, {});
			if (!readResult.has_value()) {
				std::cout << "posix: Failed to read executable" << std::endl;
				co_return Error::badExecutable;
			}
			size_t chunk = readResult.value();
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
		auto interpreterFile = FRG_CO_TRY(co_await open(root, workdir, interpreterPath, self));
		assert(interpreterFile); // If open() succeeds, it must return a non-null file.

		if(!args.empty()) // Handle exec() without arguments.
			args.erase(args.begin());
		args.insert(args.begin(), path);
		if(beginArg != endArg)
			args.insert(args.begin(), std::string{beginArg, endArg});
		args.insert(args.begin(), interpreterPath);
		path = std::move(interpreterPath);
		execFile = std::move(interpreterFile);
		nRecursions++;
	}

	auto execPreamble = FRG_CO_TRY(co_await parseElfPreamble(execFile));
	ImageInfo execInfo;
	if(execPreamble.isPie) {
		// Unconditionally apply a non-zero base address to PIE objects.
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, vmContext.get(), 0x200000));
	}else{
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, vmContext.get(), 0));
	}

	// TODO: Should we really look up the dynamic linker in the current working dir?
	auto ldsoFile = FRG_CO_TRY(co_await open(root, workdir, execInfo.interpreter, self));
	assert(ldsoFile); // If open() succeeds, it must return a non-null file.
	auto ldsoInfo = FRG_CO_TRY(co_await loadElfImage(ldsoFile, vmContext.get(), ldsoBaseAddress));

	constexpr size_t stackSize = 0x200000;

	// Allocate memory for the stack.
	auto stackArea = Area::makeAnonymous(stackSize, true);

	void *window;
	HEL_CHECK(helMapMemory(stackArea.copyView.getHandle(), kHelNullHandle, nullptr,
			0, stackSize, kHelMapProtRead | kHelMapProtWrite, &window));

	// Map the stack into the new process and set it up.
	void *stackBase = FRG_CO_TRY(vmContext->mapArea(0,
			kHelMapProtRead | kHelMapProtWrite, std::move(stackArea)));

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

	void *auxEnd = reinterpret_cast<std::byte *>(stackBase) + d;
	copyArrayToStack(window, d, (uintptr_t[]){
		AT_ENTRY,
		uintptr_t(execInfo.entryIp),
		AT_PHDR,
		uintptr_t(execInfo.phdrPtr),
		AT_PHENT,
		execInfo.phdrEntrySize,
		AT_PHNUM,
		execInfo.phdrCount,
		AT_EXECFN,
		execfn,
		AT_SECURE,
		0,
		AT_BASE,
		ldsoBaseAddress,
		AT_PAGESZ,
		0x1000,
		AT_NULL,
		0
	});
	void *auxBegin = reinterpret_cast<std::byte *>(stackBase) + d;

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
			(void *)ldsoInfo.entryIp, (char *)stackBase + d,
			kHelThreadStopped, &thread));

	co_return ExecuteResult{
		.thread = helix::UniqueDescriptor{thread},
		.auxBegin = auxBegin,
		.auxEnd = auxEnd
	};
}
