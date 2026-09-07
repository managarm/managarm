#include <elf.h>
#include <array>
#include <bit>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>
#include <limits>
#include <span>

#include "vfs.hpp"
#include "exec.hpp"
#include <fs.bragi.hpp>

constexpr size_t kPageSize = 0x1000;
constexpr uintptr_t ldsoBaseAddress = 0x40000000;

struct ValidatedElf {
	Elf64_Ehdr header;
	std::vector<Elf64_Phdr> phdrs;
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

async::result<frg::expected<Error, ValidatedElf>>
parseElf(SharedFilePtr file) {
	ValidatedElf elf;

	// Read the elf file header and verify the signature.
	FRG_CO_TRY(co_await file->seek(0, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr, &elf.header, sizeof(Elf64_Ehdr)));

	if(!(elf.header.e_ident[0] == 0x7F
			&& elf.header.e_ident[1] == 'E'
			&& elf.header.e_ident[2] == 'L'
			&& elf.header.e_ident[3] == 'F'))
		co_return Error::badExecutable;
	if(elf.header.e_type != ET_EXEC && elf.header.e_type != ET_DYN)
		co_return Error::badExecutable;

	// Read the elf program headers.
	std::vector<char> phdrBuffer;
	phdrBuffer.resize(elf.header.e_phnum * elf.header.e_phentsize);
	FRG_CO_TRY(co_await file->seek(elf.header.e_phoff, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr,
			phdrBuffer.data(), elf.header.e_phnum * size_t(elf.header.e_phentsize)));

	elf.phdrs.resize(elf.header.e_phnum);
	for(size_t i = 0; i < elf.phdrs.size(); i++)
		memcpy(&elf.phdrs[i], phdrBuffer.data() + i * elf.header.e_phentsize,
				sizeof(Elf64_Phdr));

	co_return elf;
}

async::result<frg::expected<Error, ImageInfo>>
loadElfImage(SharedFilePtr file, const ValidatedElf &elf,
		VmContext *vmContext, uintptr_t base) {
	assert(!(base & (kPageSize - 1))); // Callers need to ensure this.
	ImageInfo info;

	// Get a handle to the file's memory.
	auto fileMemory = co_await file->accessMemory();
	const auto &ehdr = elf.header;

	info.entryIp = (char *)base + ehdr.e_entry;
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// Load the parsed program headers into the address space.
	for(const auto &phdr : elf.phdrs) {

		if(phdr.p_type == PT_LOAD) {
			// The ELF gABI requires the file image of a loadable segment to
			// fit in its memory image ("Program Header", PT_LOAD).
			if(phdr.p_filesz > phdr.p_memsz)
				co_return Error::badExecutable;
			if(!phdr.p_memsz) // Skip empty segments.
				continue;

			size_t misalign = phdr.p_vaddr & (kPageSize - 1);
			constexpr auto maxSize = std::numeric_limits<size_t>::max();
			if(phdr.p_memsz > maxSize - misalign
					|| phdr.p_memsz + misalign > maxSize - (kPageSize - 1))
				co_return Error::badExecutable;

			size_t segmentSize = static_cast<size_t>(phdr.p_memsz) + misalign;
			size_t mapLength = (segmentSize + kPageSize - 1) & ~(kPageSize - 1);
			if(mapLength < misalign || phdr.p_filesz > mapLength - misalign)
				co_return Error::badExecutable;

			// The ELF gABI specifies that p_align is either 0/1 or a positive
			// integral power of two ("Program Header", p_align).
			if(phdr.p_align > 1) {
				if(!std::has_single_bit(phdr.p_align)
						|| phdr.p_offset % phdr.p_align != phdr.p_vaddr % phdr.p_align)
					co_return Error::badExecutable;
			}

			uintptr_t mapAddress = base + phdr.p_vaddr - misalign;
			uintptr_t fileOffset = phdr.p_offset - misalign;

			// Check if we can share the segment.
			if(!(phdr.p_flags & PF_W)) {
				// Map the segment with correct permissions into the process.
				if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, mapLength));

					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							fileOffset, mapLength, true,
							kHelMapProtRead | kHelMapProtExecute));
				// Allow read only mappings too, ICU loves those.
				}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R)) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, mapLength));

					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							fileOffset, mapLength, true,
							kHelMapProtRead));
				}else{
					std::cout << "posix: Illegal combination of segment permissions" << std::endl;
					co_return Error::badExecutable;
				}
			}else{
				// Map the segment with write permission into this address space.
				HelHandle segmentHandle;
				HEL_CHECK(helAllocateMemory(vmContext->getHierarchy().getHandle(), mapLength, 0, nullptr,
						&segmentHandle));

				void *window;
				HEL_CHECK(helMapMemory(segmentHandle, kHelNullHandle, nullptr,
						0, mapLength, kHelMapProtRead | kHelMapProtWrite, &window));

				// Map the segment with correct permissions into the process.
				if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							helix::UniqueDescriptor{segmentHandle}, file,
							0, mapLength, true,
							kHelMapProtRead | kHelMapProtWrite));
				}else{
					std::cout << "posix: Illegal combination of segment permissions" << std::endl;
					co_return Error::badExecutable;
				}

				// Read the segment contents from the file.
				size_t fileSize = static_cast<size_t>(phdr.p_filesz);
				memset(window, 0, mapLength);
				FRG_CO_TRY(co_await file->seek(phdr.p_offset, VfsSeek::absolute));
				FRG_CO_TRY(co_await file->readExactly(nullptr,
						(char *)window + misalign, fileSize));
				HEL_CHECK(helUnmapMemory(kHelNullHandle, window, mapLength));
			}
		}else if(phdr.p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr.p_vaddr;
		}else if(phdr.p_type == PT_INTERP) {
			info.interpreter.resize(phdr.p_filesz);
			FRG_CO_TRY(co_await file->seek(phdr.p_offset, VfsSeek::absolute));
			FRG_CO_TRY(co_await file->readExactly(nullptr,
					info.interpreter.data(), phdr.p_filesz));
			if(size_t n = info.interpreter.find('\0'); n != size_t(-1))
				info.interpreter.resize(n);
		}else if(phdr.p_type == PT_DYNAMIC || phdr.p_type == PT_TLS
				|| phdr.p_type == PT_GNU_EH_FRAME || phdr.p_type == PT_GNU_STACK
				|| phdr.p_type == PT_GNU_RELRO || phdr.p_type == PT_NOTE) {
			// Ignore this PHDR here.
		}else{
			// Ignore unknown PHDRs.
			std::cout << "posix: Unexpected PHDR type " << phdr.p_type << std::endl;
		}
	}

	co_return info;
}

void *copyWordsToStack(void *window, size_t &d, std::span<const uintptr_t> value) {
	assert(d >= alignof(uintptr_t) + sizeof(uintptr_t) * value.size());
	d -= sizeof(uintptr_t) * value.size();
	d -= d & (alignof(uintptr_t) - 1);
	void *ptr = (char *)window + d;
	memcpy(ptr, value.data(), sizeof(uintptr_t) * value.size());
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

	auto execElf = FRG_CO_TRY(co_await parseElf(execFile));
	ImageInfo execInfo;
	if(execElf.header.e_type == ET_DYN) {
		// Unconditionally apply a non-zero base address to PIE objects.
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, execElf,
				vmContext.get(), 0x200000));
	}else{
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, execElf,
				vmContext.get(), 0));
	}

	// TODO: Should we really look up the dynamic linker in the current working dir?
	auto ldsoFile = FRG_CO_TRY(co_await open(root, workdir, execInfo.interpreter, self));
	assert(ldsoFile); // If open() succeeds, it must return a non-null file.
	auto ldsoElf = FRG_CO_TRY(co_await parseElf(ldsoFile));
	auto ldsoInfo = FRG_CO_TRY(co_await loadElfImage(ldsoFile, ldsoElf,
			vmContext.get(), ldsoBaseAddress));

	auto link = execFile->associatedLink();
	if(!link) {
		co_return Error::badExecutable;
	}

	auto stats = FRG_CO_TRY(co_await link->getTarget()->getStats());
	bool hasSetuid = stats.mode & S_ISUID;
	bool hasSetgid = stats.mode & S_ISGID;
	uid_t newUid = hasSetuid ? stats.uid : self->threadGroup()->euid();
	gid_t newGid = hasSetgid ? stats.gid : self->threadGroup()->egid();

	auto makeAuxv = [&] (uintptr_t execfn) {
		return std::to_array<uintptr_t>({
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
			hasSetuid || hasSetgid,
			AT_BASE,
			ldsoBaseAddress,
			AT_PAGESZ,
			kPageSize,
			AT_NULL,
			0
		});
	};
	// The AT_EXECFN address is not known until its string is placed on the stack.
	auto auxv = makeAuxv(0);

	size_t stackImageSize = 0;
	constexpr auto maxSize = std::numeric_limits<size_t>::max();
	auto addStackSize = [&] (size_t size) {
		if(size > maxSize - stackImageSize)
			return false;
		stackImageSize += size;
		return true;
	};
	auto addStringSize = [&] (const std::string &str) {
		if(str.size() == maxSize)
			return false;
		return addStackSize(str.size() + 1);
	};

	if(!addStringSize(path))
		co_return Error::argumentListTooLong;
	for(const auto &str : args) {
		if(!addStringSize(str))
			co_return Error::argumentListTooLong;
	}
	for(const auto &str : env) {
		if(!addStringSize(str))
			co_return Error::argumentListTooLong;
	}

	// Account for string alignment and argv/envp words.
	if(!addStackSize(alignof(uintptr_t)))
		co_return Error::argumentListTooLong;
	if(!addStackSize(15))
		co_return Error::argumentListTooLong;

	// argc, the argv terminator and the envp terminator.
	size_t wordCount = 3;
	if(args.size() > maxSize - wordCount)
		co_return Error::argumentListTooLong;
	wordCount += args.size();
	if(env.size() > maxSize - wordCount)
		co_return Error::argumentListTooLong;
	wordCount += env.size();
	// Include a padding word to preserve 16-byte stack alignment.
	if(wordCount & 1) {
		if(wordCount == maxSize)
			co_return Error::argumentListTooLong;
		wordCount++;
	}
	if(wordCount > maxSize / sizeof(uintptr_t)
			|| auxv.size() > maxSize / sizeof(uintptr_t)
			|| !addStackSize(wordCount * sizeof(uintptr_t))
			|| !addStackSize(auxv.size() * sizeof(uintptr_t))
			|| stackImageSize > kExecStackSize)
		co_return Error::argumentListTooLong;

	// Allocate memory for the stack.
	HelHandle stackHandle;
	HEL_CHECK(helAllocateMemory(vmContext->getHierarchy().getHandle(), kExecStackSize,
			kHelAllocOnDemand, nullptr, &stackHandle));

	void *window;
	HEL_CHECK(helMapMemory(stackHandle, kHelNullHandle, nullptr,
			0, kExecStackSize, kHelMapProtRead | kHelMapProtWrite, &window));

	// Map the stack into the new process and set it up.
	void *stackBase = FRG_CO_TRY(co_await vmContext->mapFile(0,
			helix::UniqueDescriptor{stackHandle}, nullptr,
			0, kExecStackSize, true, kHelMapProtRead | kHelMapProtWrite));

	// the offset at which the stack image starts.
	size_t d = kExecStackSize;

	// Copy argument and environment strings to the stack.
	auto pushString = [&] (const std::string &str) -> uintptr_t {
		d -= str.size() + 1;
		memcpy(reinterpret_cast<char *>(window) + d, str.c_str(), str.size() + 1);
		return reinterpret_cast<uintptr_t>(stackBase) + d;
	};

	auto execfn = pushString(path);
	auxv = makeAuxv(execfn);
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
	copyWordsToStack(window, d, auxv);
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

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, kExecStackSize));

	HelHandle thread;
	HEL_CHECK(helCreateThread(universe.getHandle(),
			vmContext->getSpace().getHandle(), kHelAbiSystemV,
			(void *)ldsoInfo.entryIp, (char *)stackBase + d,
			kHelThreadStopped, &thread));

	co_return ExecuteResult{
		.thread = helix::UniqueDescriptor{thread},
		.auxBegin = auxBegin,
		.auxEnd = auxEnd,
		.effectiveUid = newUid,
		.effectiveGid = newGid,
		.savedUid = self->threadGroup()->uid(),
		.savedGid = self->threadGroup()->gid()
	};
}
