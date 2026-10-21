#include <elf.h>
#include <algorithm>
#include <array>
#include <bit>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>
#include <limits>
#include <optional>
#include <span>

#include "vfs.hpp"
#include "exec.hpp"
#include <fs.bragi.hpp>

constexpr size_t kPageSize = 0x1000;
constexpr uintptr_t ldsoBaseAddress = 0x40000000;
// Extended program-header counts require section-header parsing, which the
// loader does not implement.
constexpr size_t kMaxProgramHeaders = 1024;
constexpr size_t kMaxInterpreterSize = 4096;
constexpr size_t kMaxShebangSize = 128;

#if defined(__x86_64__)
constexpr uint16_t kElfMachine = EM_X86_64;
#elif defined(__aarch64__)
constexpr uint16_t kElfMachine = EM_AARCH64;
#elif defined(__riscv) && __riscv_xlen == 64
constexpr uint16_t kElfMachine = EM_RISCV;
#else
#error "Unsupported architecture"
#endif

template<typename T>
bool checkedAdd(T a, T b, T &result) {
	if(b > std::numeric_limits<T>::max() - a)
		return false;
	result = a + b;
	return true;
}

template<typename T>
bool checkedMultiply(T a, T b, T &result) {
	if(a && b > std::numeric_limits<T>::max() / a)
		return false;
	result = a * b;
	return true;
}

struct ValidatedProgramHeader {
	Elf64_Phdr header;
	size_t misalign = 0;
	size_t fileOffset = 0;
	size_t mapLength = 0;
};

struct ValidatedElf {
	Elf64_Ehdr header;
	std::vector<ValidatedProgramHeader> phdrs;
	std::optional<uint64_t> phdrVaddr;
	std::optional<std::string> interpreter;
};

// This struct contains the image meta data with correct base address applied.
struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr), phdrPtr(nullptr), phdrEntrySize(0), phdrCount(0) { }

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

	if(!(elf.header.e_ident[EI_MAG0] == ELFMAG0
			&& elf.header.e_ident[1] == 'E'
			&& elf.header.e_ident[2] == 'L'
			&& elf.header.e_ident[EI_MAG3] == ELFMAG3
			&& elf.header.e_ident[EI_CLASS] == ELFCLASS64
			&& elf.header.e_ident[EI_DATA] == ELFDATA2LSB
			&& elf.header.e_ident[EI_VERSION] == EV_CURRENT
			&& elf.header.e_version == EV_CURRENT
			&& elf.header.e_machine == kElfMachine
			&& elf.header.e_ehsize == sizeof(Elf64_Ehdr)
			&& elf.header.e_phentsize == sizeof(Elf64_Phdr)))
		co_return Error::badExecutable;

	if(elf.header.e_type != ET_EXEC && elf.header.e_type != ET_DYN)
		co_return Error::badExecutable;

	if(!elf.header.e_phnum || elf.header.e_phnum == PN_XNUM
			|| elf.header.e_phnum > kMaxProgramHeaders)
		co_return Error::badExecutable;

	auto link = file->associatedLink();
	if(!link)
		co_return Error::badExecutable;

	auto stats = FRG_CO_TRY(co_await link->getTarget()->getStats());
	uint64_t fileSize = stats.fileSize;

	// Read the elf program headers.
	constexpr uint64_t maxFileOffset = std::numeric_limits<off_t>::max();
	uint64_t phdrSize;
	if(!checkedMultiply(uint64_t(elf.header.e_phnum),
			uint64_t(elf.header.e_phentsize), phdrSize))
		co_return Error::badExecutable;
	if(elf.header.e_phoff > fileSize)
		co_return Error::badExecutable;
	if(phdrSize > fileSize - elf.header.e_phoff)
		co_return Error::badExecutable;
	if(phdrSize > maxFileOffset - elf.header.e_phoff)
		co_return Error::badExecutable;

	std::vector<char> phdrBuffer;
	phdrBuffer.resize(static_cast<size_t>(phdrSize));
	FRG_CO_TRY(co_await file->seek(elf.header.e_phoff, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr,
			phdrBuffer.data(), phdrBuffer.size()));

	bool hasLoadSegment = false;
	bool ambiguousPhdr = false;
	bool hasInterpreter = false;
	std::optional<Elf64_Phdr> explicitPhdr;
	for(size_t i = 0; i < elf.header.e_phnum; i++) {
		ValidatedProgramHeader validated;
		memcpy(&validated.header, phdrBuffer.data() + i * sizeof(Elf64_Phdr),
				sizeof(Elf64_Phdr));
		auto &phdr = validated.header;
		if(phdr.p_type == PT_NULL)
			continue;

		uint64_t fileEnd;
		if(phdr.p_offset > fileSize
				|| !checkedAdd(phdr.p_offset, phdr.p_filesz, fileEnd)
				|| fileEnd > fileSize
				|| phdr.p_offset > maxFileOffset
				|| fileEnd > maxFileOffset
				|| phdr.p_filesz > std::numeric_limits<size_t>::max())
			co_return Error::badExecutable;

		if(phdr.p_type == PT_LOAD) {
			if(phdr.p_filesz > phdr.p_memsz)
				co_return Error::badExecutable;

			if(phdr.p_align > 1
					&& (!std::has_single_bit(phdr.p_align)
							|| phdr.p_offset % phdr.p_align != phdr.p_vaddr % phdr.p_align))
				co_return Error::badExecutable;

			if(!phdr.p_memsz)
				continue;
			hasLoadSegment = true;

			if(phdr.p_memsz > std::numeric_limits<uint64_t>::max() - phdr.p_vaddr)
				co_return Error::badExecutable;

			validated.misalign = phdr.p_vaddr & (kPageSize - 1);
			if(phdr.p_offset < validated.misalign)
				co_return Error::badExecutable;

			uint64_t segmentSize;
			if(!checkedAdd(phdr.p_memsz, uint64_t(validated.misalign), segmentSize)
					|| segmentSize > std::numeric_limits<uint64_t>::max() - (kPageSize - 1))
				co_return Error::badExecutable;

			uint64_t mapLength = (segmentSize + kPageSize - 1)
					& ~(uint64_t(kPageSize) - 1);
			if(mapLength < validated.misalign
					|| phdr.p_filesz > mapLength - validated.misalign
					|| mapLength > std::numeric_limits<size_t>::max()
					|| mapLength > std::numeric_limits<size_t>::max() - (kPageSize - 1))
				co_return Error::badExecutable;

			uint64_t fileOffset = phdr.p_offset - validated.misalign;
			uint64_t mappingEnd;
			if(!checkedAdd(fileOffset, mapLength, mappingEnd)
					|| mappingEnd > std::numeric_limits<size_t>::max()
					|| mappingEnd > uint64_t(std::numeric_limits<intptr_t>::max()))
				co_return Error::badExecutable;

			validated.fileOffset = static_cast<size_t>(fileOffset);
			validated.mapLength = static_cast<size_t>(mapLength);

			if((phdr.p_flags & PF_R) && elf.header.e_phoff >= phdr.p_offset) {
				uint64_t tableOffset = elf.header.e_phoff - phdr.p_offset;
				if(tableOffset <= phdr.p_filesz
						&& phdrSize <= phdr.p_filesz - tableOffset) {
					uint64_t phdrVaddr;
					if(!checkedAdd(phdr.p_vaddr, tableOffset, phdrVaddr))
						co_return Error::badExecutable;
					if(elf.phdrVaddr) {
						if(*elf.phdrVaddr != phdrVaddr)
							ambiguousPhdr = true;
					}else{
						elf.phdrVaddr = phdrVaddr;
					}
				}
			}
		}

		if(phdr.p_type == PT_PHDR) {
			if(explicitPhdr)
				co_return Error::badExecutable;
			explicitPhdr = phdr;
		}

		if(phdr.p_type == PT_INTERP) {
			if(hasInterpreter || !phdr.p_filesz
					|| phdr.p_filesz > kMaxInterpreterSize)
				co_return Error::badExecutable;

			char interpreter[kMaxInterpreterSize];
			FRG_CO_TRY(co_await file->seek(phdr.p_offset, VfsSeek::absolute));
			FRG_CO_TRY(co_await file->readExactly(nullptr, interpreter,
					static_cast<size_t>(phdr.p_filesz)));
			const char *nul = static_cast<const char *>(memchr(interpreter,
					'\0', static_cast<size_t>(phdr.p_filesz)));
			if(!nul || nul == interpreter)
				co_return Error::badExecutable;
			elf.interpreter.emplace(interpreter, nul - interpreter);
			hasInterpreter = true;
		}
		elf.phdrs.push_back(std::move(validated));
	}

	if(!hasLoadSegment || ambiguousPhdr)
		co_return Error::badExecutable;
	if(explicitPhdr && (explicitPhdr->p_offset != elf.header.e_phoff
				|| explicitPhdr->p_filesz < phdrSize
				|| explicitPhdr->p_memsz < explicitPhdr->p_filesz
				|| !elf.phdrVaddr || explicitPhdr->p_vaddr != *elf.phdrVaddr))
		co_return Error::badExecutable;

	co_return elf;
}

async::result<frg::expected<Error, ImageInfo>>
loadElfImage(SharedFilePtr file, const ValidatedElf &elf,
		VmContext *vmContext, uintptr_t base) {
	assert(!(base & (kPageSize - 1))); // Callers need to ensure this.
	ImageInfo info;
	bool hasPhdr = false;

	// Get a handle to the file's memory.
	auto fileMemory = co_await file->accessMemory();
	const auto &ehdr = elf.header;

	auto addBase = [&] (uint64_t address, uintptr_t &result) {
		if(address > std::numeric_limits<uintptr_t>::max() - base)
			return false;
		result = base + static_cast<uintptr_t>(address);
		return true;
	};
	uintptr_t entryIp;
	if(!addBase(ehdr.e_entry, entryIp))
		co_return Error::badExecutable;
	info.entryIp = reinterpret_cast<void *>(entryIp);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// Load the parsed program headers into the address space.
	for(const auto &validated : elf.phdrs) {
		const auto &phdr = validated.header;

		if(phdr.p_type == PT_LOAD) {
			if(!phdr.p_memsz) // Skip empty segments.
				continue;

			uintptr_t segmentAddress;
			if(!addBase(phdr.p_vaddr, segmentAddress)
					|| segmentAddress < validated.misalign
					|| validated.mapLength > std::numeric_limits<uintptr_t>::max()
							- (segmentAddress - validated.misalign))
				co_return Error::badExecutable;
			uintptr_t mapAddress = segmentAddress - validated.misalign;
			size_t mapLength = validated.mapLength;
			size_t fileOffset = validated.fileOffset;

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
						(char *)window + validated.misalign, fileSize));
				HEL_CHECK(helUnmapMemory(kHelNullHandle, window, mapLength));
			}
		}else if(phdr.p_type == PT_INTERP) {
			if(!elf.interpreter)
				co_return Error::badExecutable;
			info.interpreter = *elf.interpreter;
		}else if(phdr.p_type == PT_PHDR) {
			uintptr_t phdrPtr;
			if(!addBase(phdr.p_vaddr, phdrPtr))
				co_return Error::badExecutable;
			info.phdrPtr = reinterpret_cast<void *>(phdrPtr);
			hasPhdr = true;
		}else if(phdr.p_type == PT_DYNAMIC || phdr.p_type == PT_TLS
				|| phdr.p_type == PT_GNU_EH_FRAME || phdr.p_type == PT_GNU_STACK
				|| phdr.p_type == PT_GNU_RELRO || phdr.p_type == PT_NOTE) {
			// Ignore this PHDR here.
		}else{
			// Ignore unknown PHDRs.
			std::cout << "posix: Unexpected PHDR type " << phdr.p_type << std::endl;
		}
	}
	if(!hasPhdr && elf.phdrVaddr) {
		uintptr_t phdrPtr;
		if(!addBase(*elf.phdrVaddr, phdrPtr))
			co_return Error::badExecutable;
		info.phdrPtr = reinterpret_cast<void *>(phdrPtr);
		hasPhdr = true;
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
	std::string processName = path.substr(path.rfind('/') + 1);

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
		if(shebangPrefix[0] != '#' || shebangPrefix[1] != '!')
			break;

		std::string shebangStr;
		while(true) {
			if(shebangStr.size() >= kMaxShebangSize) {
				std::cout << "posix: Shebang line of excessive length" << std::endl;
				co_return Error::badExecutable;
			}

			char buffer[128];
			// TODO: Loop until EOF or the requested size is read.
			auto readResult = co_await execFile->readSome(nullptr, buffer,
					std::min(sizeof(buffer), kMaxShebangSize - shebangStr.size()), {});
			if (!readResult.has_value()) {
				std::cout << "posix: Failed to read executable" << std::endl;
				co_return Error::badExecutable;
			}
			size_t chunk = readResult.value();
			if(!chunk) {
				std::cout << "posix: EOF in shebang line" << std::endl;
				co_return Error::badExecutable;
			}
			auto nlPtr = std::find(buffer, buffer + chunk, '\n');
			shebangStr.insert(shebangStr.end(), buffer, nlPtr);
			if(nlPtr != buffer + chunk)
				break;
		}

		// The path is the first whitespace-separated word of the line.
		// Trim spaces from the left and the right.
		auto beginPath = std::find_if_not(shebangStr.begin(), shebangStr.end(), isspace);
		if(beginPath == shebangStr.end())
			co_return Error::badExecutable;
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
		.executablePath = ViewPath{execFile->associatedMount(), execFile->associatedLink()}.getPath(root),
		.processName = std::move(processName),
		.auxBegin = auxBegin,
		.auxEnd = auxEnd,
		.effectiveUid = newUid,
		.effectiveGid = newGid,
		.savedUid = newUid,
		.savedGid = newGid
	};
}
