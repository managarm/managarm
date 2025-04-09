#include <linux/elf.h>
#include <numeric>
#include <print>
#include <sys/auxv.h>

#include "process.hpp"

namespace {

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> createFile(Process *proc, std::string name) {
	PathResolver resolver;
	resolver.setup(proc->fsContext()->getRoot(), proc->fsContext()->getWorkingDirectory(), name, proc);
	auto resolveResult = co_await resolver.resolve(
		resolvePrefix | resolveNoTrailingSlash);
	assert(resolveResult);

	auto directory = resolver.currentLink()->getTarget();
	auto linkResult = co_await directory->getLinkOrCreate(proc, name, 0755);
	if (!linkResult)
		co_return linkResult.error();

	auto link = linkResult.value();
	auto fileResult = co_await link->getTarget()->open(resolver.currentView(), std::move(link), semanticRead | semanticWrite);
	co_return fileResult;
}

size_t ntSiginfoSize() {
	return sizeof(siginfo_t);
}

async::result<size_t> ntAuxvSize(Process *p, std::vector<std::byte> &buffer) {
	auto begin = reinterpret_cast<std::byte *>(p->clientAuxBegin());
	auto end = reinterpret_cast<std::byte *>(p->clientAuxEnd());
	for(auto it = begin; it != end; ++it) {
		// We load the memory byte for byte until we fail,
		// readMemory does not support partial reads yet.
		std::byte b;
		auto loadMemory = co_await helix_ng::readMemory(p->vmContext()->getSpace(),
			reinterpret_cast<uintptr_t>(it), 1, &b);
		if(loadMemory.error())
			break;
		buffer.push_back(b);
	}

	co_return buffer.size();
}

size_t ntFileSize(Process *p, std::vector<std::string> &noteFiles) {
	size_t noteSize = sizeof(long) + sizeof(long);

	for(auto area : *p->vmContext()) {
		if(!area.backingFile())
			continue;
		noteSize += 3 * sizeof(long);
		ViewPath path{area.backingFile()->associatedMount(), area.backingFile()->associatedLink()};
		noteFiles.push_back(path.getPath(p->fsContext()->getRoot()));
		noteSize += noteFiles.back().size() + 1;
	}

	return noteSize;
}

size_t ntSizeAlign(size_t noteSize) {
	return (noteSize + 3) & ~3;
}

static const auto simdStateSize = [] () -> size_t {
	HelRegisterInfo regInfo;
	HEL_CHECK(helQueryRegisterInfo(kHelRegsSimd, &regInfo));
	return regInfo.setSize;
}();

bool dumpArea(VmContext::AreaAccessor &area) {
	return !area.backingFile() || (area.isWritable() || area.isExecutable());
}

struct prstatus {
	int si_signo;
	int si_code;
	int si_errno;
	short pr_cursig;
	unsigned long pr_sigpend;
	unsigned long pr_sighold;
	int pr_pid;
	int pr_ppid;
	int pr_pgrp;
	int pr_sid;
	timeval pr_utime;
	timeval pr_stime;
	timeval pr_cutime;
	timeval pr_cstime;
} prstatus;

struct prpsinfo {
	char pr_state;
	char pr_sname;
	char pr_zomb;
	char pr_nice;
	unsigned long pr_flag;
	int pr_uid;
	int pr_gid;
	int pr_pid;
	int pr_ppid;
	int pr_pgrp;
	int pr_sid;
	char pr_fname[16];
	char pr_psargs[80];
};

#if defined(__x86_64__)
struct regs {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long bp;
	unsigned long bx;
	unsigned long r11;
	unsigned long r10;
	unsigned long r9;
	unsigned long r8;
	unsigned long ax;
	unsigned long cx;
	unsigned long dx;
	unsigned long si;
	unsigned long di;
	unsigned long orig_ax;
	unsigned long ip;
	unsigned long cs;
	unsigned long flags;
	unsigned long sp;
	unsigned long ss;
	unsigned long fs_base;
	unsigned long gs_base;
	unsigned long ds;
	unsigned long es;
	unsigned long fs;
	unsigned long gs;
	int pr_fpvalid = 0;
};

struct fpregs {
	unsigned short cwd;
	unsigned short swd;
	unsigned short twd;
	unsigned short fop;
	uint64_t rip;
	uint64_t rdp;
	uint32_t mxcsr;
	uint32_t mxcr_mask;
	uint32_t st_space[32];
	uint32_t xmm_space[64];
	uint32_t padding[24];
};
#endif

}

async::result<void> Process::coredump(TerminationState state) {
	if(!dumpable_)
		co_return;

	std::println("posix: writing coredump for process {}", pid());
	auto fileResult = co_await createFile(this, std::format("core.{}", pid()));
	if(!fileResult)
		co_return;
	auto file = std::move(fileResult.value());

	Elf64_Half phdrCount = 1;
	size_t memoryDumpSize = 0;

	for(auto area : *vmContext()) {
		phdrCount++;
		if(dumpArea(area))
			memoryDumpSize += area.size();
	}

	size_t noteSize = 0;
	std::vector<std::byte> auxv;
	std::vector<std::string> noteFiles;

	// NT_PRSTATUS
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(prstatus) + sizeof(regs);
	// NT_PRPSINFO
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(prpsinfo);
	// NT_SIGINFO
	noteSize += sizeof(Elf64_Nhdr) + 8 + ntSiginfoSize();
	// NT_AUXV
	noteSize += sizeof(Elf64_Nhdr) + 8 + co_await ntAuxvSize(this, auxv);
	// NT_FILE
	noteSize += sizeof(Elf64_Nhdr) + 8 + ntFileSize(this, noteFiles);
	noteSize = ntSizeAlign(noteSize);
	// NT_PRFPREG
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(fpregs);

	size_t memoryDumpOffset = sizeof(Elf64_Ehdr) + (phdrCount * sizeof(Elf64_Phdr)) + noteSize;
	memoryDumpOffset = (memoryDumpOffset + 0x1000 - 1) & ~0xFFF;

	auto resizeResult = co_await file->truncate(memoryDumpOffset + memoryDumpSize);
	assert(resizeResult);

	auto memory = co_await file->accessMemory();
	assert(memory);

	void *memoryDump = nullptr;
	HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle,
		nullptr, 0, memoryDumpOffset + memoryDumpSize,
		kHelMapProtRead | kHelMapProtWrite, &memoryDump));

	size_t fileOffset = 0;

	auto write = [&memoryDump, &fileOffset](const void *data, size_t len) {
		memcpy(reinterpret_cast<void *>(uintptr_t(memoryDump) + fileOffset), data, len);
		fileOffset += len;
	};

	Elf64_Ehdr hdr{
		.e_type = ET_CORE,
		.e_machine = EM_X86_64,
		.e_version = EV_CURRENT,
		.e_phoff = sizeof(Elf64_Ehdr),
		.e_flags = 0,
		.e_ehsize = sizeof(Elf64_Ehdr),
		.e_phentsize = sizeof(Elf64_Phdr),
		.e_phnum = phdrCount,
	};

	memcpy(hdr.e_ident, ELFMAG, SELFMAG);
	hdr.e_ident[EI_CLASS] = ELFCLASS64;
	hdr.e_ident[EI_DATA] = ELFDATA2LSB;
	hdr.e_ident[EI_VERSION] = EV_CURRENT;

	write(&hdr, sizeof(hdr));

	Elf64_Phdr notePhdr{
		.p_type = PT_NOTE,
		.p_flags = 0,
		.p_offset = sizeof(hdr) + (hdr.e_phnum * sizeof(Elf64_Phdr)),
		.p_filesz = noteSize,
		.p_align = 4,
	};

	write(&notePhdr, sizeof(notePhdr));

	size_t dumpRegionOffset = 0;
	for(auto area : *vmContext()) {
		Elf64_Word areaFlags = 0;
		if(area.isReadable())
			areaFlags |= PF_R;
		if(area.isWritable())
			areaFlags |= PF_W;
		if(area.isExecutable())
			areaFlags |= PF_X;

		bool dump = dumpArea(area);

		Elf64_Phdr phdr{
			.p_type = PT_LOAD,
			.p_flags = areaFlags,
			.p_offset = memoryDumpOffset + dumpRegionOffset,
			.p_vaddr = area.baseAddress(),
			.p_paddr = 0,
			.p_filesz = dump ? area.size() : 0,
			.p_memsz = area.size(),
			.p_align = 0x1000,
		};

		dumpRegionOffset += phdr.p_filesz;

		write(&phdr, sizeof(phdr));
	}

	// NT_PRSTATUS
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRSTATUS,
		};
		noteHeader.n_descsz = sizeof(prstatus) + sizeof(regs);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
			prstatus.si_signo = bySignal->signo;
			prstatus.si_code = SI_TKILL;
		}

		prstatus.pr_pid = pid();
		prstatus.pr_ppid = _parent ? _parent->pid() : 0;
		write(&prstatus, sizeof(prstatus));

		uintptr_t pcrs[2];
		uintptr_t threadrs[2];
		uintptr_t gprs[kHelNumGprs];

		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
				kHelRegsProgram, pcrs));
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
				kHelRegsGeneral, gprs));
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
			kHelRegsThread, threadrs));

		#if defined(__x86_64__)
		regs reg;

		reg.ax = gprs[0];
		reg.bx = gprs[1];
		reg.cx = gprs[2];
		reg.dx = gprs[3];
		reg.si = gprs[5];
		reg.di = gprs[4];
		reg.bp = gprs[14];
		reg.sp = pcrs[1];
		reg.r8 = gprs[6];
		reg.r9 = gprs[7];
		reg.r10 = gprs[8];
		reg.r11 = gprs[9];
		reg.r12 = gprs[10];
		reg.r13 = gprs[11];
		reg.r14 = gprs[12];
		reg.r15 = gprs[13];
		reg.ip = pcrs[0];
		reg.fs = threadrs[0];
		reg.gs = threadrs[1];

		write(&reg, sizeof(reg));
		#else
		#warning "Unsupported architecture for coredump"
		#endif
	}

	// NT_PRPSINFO
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRPSINFO,
		};
		noteHeader.n_descsz = sizeof(prpsinfo);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		prpsinfo info;
		memset(&info, 0, sizeof(info));
		info.pr_sname = 'R';
		info.pr_pid = pid();
		info.pr_ppid = _parent ? _parent->pid() : 0;
		info.pr_uid = uid();
		info.pr_gid = gid();
		info.pr_flag = 0x600;
		strncpy(info.pr_fname, _name.c_str(), sizeof(info.pr_fname));
		strncpy(info.pr_psargs, _path.c_str(), sizeof(info.pr_psargs));
		write(&info, sizeof(info));
	}

	// NT_SIGINFO
	{
		siginfo_t siginfo{};

		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_SIGINFO,
		};
		noteHeader.n_descsz = sizeof(siginfo);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
			siginfo.si_signo = bySignal->signo;
			siginfo.si_code = SI_TKILL;
		}

		write(&siginfo, sizeof(siginfo));
	}

	// NT_AUXV
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_AUXV,
		};
		noteHeader.n_descsz = auxv.size();
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));
		write(auxv.data(), auxv.size());
	}

	// NT_FILE
	{
		size_t noteFileDesc = std::accumulate(noteFiles.begin(), noteFiles.end(), 0,
			[&](size_t sum, const std::string &file) {
				return sum + file.size() + 1;
			}) + (3 * sizeof(long) * noteFiles.size());

		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_FILE,
		};
		noteHeader.n_descsz = (2 * sizeof(long)) + noteFileDesc;
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		long info[2];
		info[0] = noteFiles.size();
		info[1] = 0x1000;

		write(&info, sizeof(info));

		for(auto area : *vmContext()) {
			if(!area.backingFile())
				continue;

			long fileInfo[3];
			fileInfo[0] = area.baseAddress();
			fileInfo[1] = area.baseAddress() + area.size();
			fileInfo[2] = area.backingFileOffset() / 0x1000;
			write(&fileInfo, sizeof(fileInfo));
		}

		for(auto path : noteFiles) {
			write(path.c_str(), path.size() + 1);
		}

		size_t disalign = (4 - (noteFileDesc % 4)) & 3;
		fileOffset += disalign;
	}

	// NT_PRFPREG
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRFPREG,
		};
		noteHeader.n_descsz = sizeof(fpregs);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		std::vector<std::byte> fpreg(simdStateSize);
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
			kHelRegsSimd, fpreg.data()));
		write(fpreg.data(), sizeof(struct fpregs));
	}

	fileOffset = memoryDumpOffset;


	for(auto area : *vmContext()) {
		if(!dumpArea(area))
			continue;

		auto loadMemory = co_await helix_ng::readMemory(vmContext()->getSpace(),
			area.baseAddress(), area.size(),
			reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(memoryDump) + fileOffset));
		assert(!loadMemory.error());

		fileOffset += area.size();
	}

	HEL_CHECK(helUnmapMemory(kHelNullHandle, memoryDump, memoryDumpOffset + memoryDumpSize));

	assert(fileOffset == (memoryDumpOffset + memoryDumpSize));

	co_return;
}
