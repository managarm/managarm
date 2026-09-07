#include <assert.h>
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "testsuite.hpp"

namespace {

using ElfMutator = void (*)(Elf64_Ehdr &, Elf64_Phdr &);
using InterpreterMutator = void (*)(Elf64_Phdr &);
using ProgramHeaderMutator = void (*)(Elf64_Ehdr &, std::vector<Elf64_Phdr> &);

#if defined(__linux__)
constexpr int kMalformedElfSignal = SIGSEGV;
#else
constexpr int kMalformedElfSignal = 0;
#endif

void expectExecError(uint64_t filesz, uint64_t memsz, uint64_t align,
		const char *interpreter, int expectedError, bool validMagic = true,
		uint16_t type = ET_EXEC, ElfMutator mutate = nullptr,
		int expectedSignal = 0, InterpreterMutator mutateInterpreter = nullptr,
		size_t extraProgramHeaders = 0, ProgramHeaderMutator mutateProgramHeaders = nullptr) {
	uint16_t machine;
#if defined(__x86_64__)
	machine = EM_X86_64;
#elif defined(__aarch64__)
	machine = EM_AARCH64;
#elif defined(__riscv) && __riscv_xlen == 64
	machine = EM_RISCV;
#else
	skip_test("unsupported architecture");
#endif

	char path[] = "/tmp/posix-tests-exec-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);

	constexpr size_t segmentOffset = 0x1000;
	constexpr size_t interpreterOffset = 0x2000;
	size_t interpreterSize = interpreter ? strlen(interpreter) + 1 : 0;
	size_t fileSize = interpreter ? interpreterOffset + interpreterSize
			: segmentOffset + filesz;
	std::vector<char> image(fileSize);

	Elf64_Ehdr ehdr{};
	if(validMagic)
		ehdr.e_ident[EI_MAG0] = ELFMAG0;
	ehdr.e_ident[EI_MAG1] = ELFMAG1;
	ehdr.e_ident[EI_MAG2] = ELFMAG2;
	ehdr.e_ident[EI_MAG3] = ELFMAG3;
	ehdr.e_ident[EI_CLASS] = ELFCLASS64;
	ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_type = type;
	ehdr.e_machine = machine;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_phoff = sizeof(Elf64_Ehdr);
	const size_t programHeaderOffset = ehdr.e_phoff;
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = (interpreter ? 2 : 1) + extraProgramHeaders;

	Elf64_Phdr phdr{};
	phdr.p_type = PT_LOAD;
	phdr.p_flags = PF_R | PF_W;
	phdr.p_offset = segmentOffset;
	phdr.p_vaddr = 0;
	phdr.p_filesz = filesz;
	phdr.p_memsz = memsz;
	phdr.p_align = align;
	std::vector<Elf64_Phdr> programHeaders(ehdr.e_phnum);
	programHeaders[0] = phdr;

	memcpy(image.data(), &ehdr, sizeof(ehdr));
	memcpy(image.data() + programHeaderOffset, &phdr, sizeof(phdr));
	if(interpreter) {
		Elf64_Phdr interpPhdr{};
		interpPhdr.p_type = PT_INTERP;
		interpPhdr.p_offset = interpreterOffset;
		interpPhdr.p_filesz = interpreterSize;
		if(mutateInterpreter)
			mutateInterpreter(interpPhdr);
		programHeaders[1] = interpPhdr;
		memcpy(image.data() + programHeaderOffset + sizeof(phdr),
				&interpPhdr, sizeof(interpPhdr));
		memcpy(image.data() + interpreterOffset, interpreter, interpreterSize);
	}
	if(mutate) {
		mutate(ehdr, phdr);
		memcpy(image.data(), &ehdr, sizeof(ehdr));
		memcpy(image.data() + sizeof(Elf64_Ehdr), &phdr, sizeof(phdr));
	}
	programHeaders[0] = phdr;
	if(mutateProgramHeaders)
		mutateProgramHeaders(ehdr, programHeaders);
	memcpy(image.data(), &ehdr, sizeof(ehdr));
	memcpy(image.data() + programHeaderOffset, programHeaders.data(),
			programHeaders.size() * sizeof(Elf64_Phdr));

	size_t offset = 0;
	while(offset < image.size()) {
		ssize_t n = write(fd, image.data() + offset, image.size() - offset);
		assert(n > 0);
		offset += n;
	}
	assert(fchmod(fd, 0700) == 0);
	assert(close(fd) == 0);

	pid_t pid = fork();
	assert_errno("fork", pid >= 0);
	if(!pid) {
		char *const args[] = {path, nullptr};
		execve(path, args, nullptr);
		int error = errno;
		_exit(error == expectedError ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	int status = 0;
	while(waitpid(pid, &status, 0) == -1) {
		if(errno == EINTR)
			continue;
		assert_errno("waitpid", false);
	}
	if(expectedSignal) {
		assert(WIFSIGNALED(status));
		assert(WTERMSIG(status) == expectedSignal);
	}else {
		assert(WIFEXITED(status));
		assert(WEXITSTATUS(status) == EXIT_SUCCESS);
	}
	assert(unlink(path) == 0);
}

}

DEFINE_TEST(exec_rejects_writable_load_with_filesz_larger_than_memsz, ([] {
#if defined(__linux__)
	// The ELF gABI requires p_filesz <= p_memsz for PT_LOAD. Linux checks
	// this only after begin_new_exec(), so the malformed image terminates
	// with SIGSEGV instead of returning ENOEXEC. Test this on Managarm,
	// whose loader rejects the image before replacing the old process image.
	skip_test("Linux rejects malformed ELF after replacing the process image");
#else
	expectExecError(0x1001, 1, 0x1000, nullptr, ENOEXEC);
#endif
}))

DEFINE_TEST(exec_accepts_zero_load_alignment, ([] {
	expectExecError(1, 1, 0, "/does/not/exist", ENOENT);
	expectExecError(1, 1, 1, "/does/not/exist", ENOENT);
}))

DEFINE_TEST(exec_accepts_elf_without_pt_phdr, ([] {
	expectExecError(0x1000, 0x1000, 0, "/does/not/exist", ENOENT, true, ET_EXEC,
		[](Elf64_Ehdr &, Elf64_Phdr &phdr) {
			phdr.p_offset = 0;
		});
}))

DEFINE_TEST(exec_ignores_unused_program_headers, ([] {
	expectExecError(1, 1, 0, "/does/not/exist", ENOENT, true, ET_EXEC,
		nullptr, 0, nullptr, 1, [](Elf64_Ehdr &, std::vector<Elf64_Phdr> &phdrs) {
			phdrs[2].p_offset = UINT64_MAX;
			phdrs[2].p_filesz = UINT64_MAX;
		});
}))

DEFINE_TEST(exec_rejects_invalid_pt_interp, ([] {
	expectExecError(1, 1, 0, "/does/not/exist", ENOEXEC, true, ET_EXEC,
		nullptr, 0, [](Elf64_Phdr &phdr) {
			phdr.p_filesz = 0;
		});
	expectExecError(1, 1, 0, "\0", ENOEXEC);
	expectExecError(1, 1, 0, "/does/not/exist", ENOEXEC, true, ET_EXEC,
		nullptr, 0, [](Elf64_Phdr &phdr) {
			phdr.p_filesz = strlen("/does/not/exist");
		});
#if !defined(__linux__)
	expectExecError(1, 1, 0, "/does/not/exist", ENOEXEC, true, ET_EXEC,
		nullptr, 0, [](Elf64_Phdr &phdr) {
			phdr.p_filesz = UINT64_MAX;
		});
#endif

	std::string oversized(4096, 'x');
	expectExecError(1, 1, 0, oversized.c_str(), ENOEXEC);

#if !defined(__linux__)
	expectExecError(1, 1, 0, "/does/not/exist", ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &, Elf64_Phdr &phdr) {
			phdr.p_type = PT_INTERP;
			phdr.p_offset = 0x2000;
			phdr.p_filesz = sizeof("/does/not/exist");
		});
#endif
}))

DEFINE_TEST(exec_rejects_non_power_of_two_load_alignment, ([] {
#if defined(__linux__)
	// The ELF gABI specifies that p_align must be 0/1 or a positive power
	// of two. Linux ignores this invalid alignment, starts the malformed
	// image, and the child dies with SIGSEGV instead of returning ENOEXEC.
	skip_test("Linux ignores malformed ELF alignment after loading the image");
#else
	expectExecError(1, 1, 3, nullptr, ENOEXEC);
#endif
}))

DEFINE_TEST(exec_rejects_invalid_elf_magic, ([] {
	expectExecError(0, 0, 0, nullptr, ENOEXEC, false);
}))

DEFINE_TEST(exec_rejects_invalid_elf_type, ([] {
	expectExecError(0, 0, 0, nullptr, ENOEXEC, true, ET_NONE);
}))

DEFINE_TEST(exec_rejects_invalid_elf_metadata, ([] {
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_ident[EI_CLASS] = ELFCLASS32;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_ident[EI_VERSION] = EV_NONE;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_version = EV_NONE;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_machine = EM_NONE;
		});
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_ehsize--;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phentsize--;
		});
}))

DEFINE_TEST(exec_rejects_invalid_elf_program_header_table, ([] {
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phnum = 0;
		});
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phoff = UINT64_MAX;
		});
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phoff = 4090;
		});
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phnum = PN_XNUM;
		});
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &ehdr, Elf64_Phdr &) {
			ehdr.e_phnum = 1025;
		});
}))

DEFINE_TEST(exec_rejects_invalid_elf_segment_ranges, ([] {
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &, Elf64_Phdr &phdr) {
			phdr.p_offset = 4090;
			phdr.p_filesz = 8;
			phdr.p_memsz = 8;
		}, kMalformedElfSignal);
	expectExecError(1, 1, 0, nullptr, ENOEXEC, true, ET_EXEC,
		[](Elf64_Ehdr &, Elf64_Phdr &phdr) {
			phdr.p_vaddr = UINT64_MAX;
			phdr.p_memsz = 1;
		}, kMalformedElfSignal);
}))

DEFINE_TEST(exec_rejects_oversized_argument_list, ([] {
	std::vector<char> argument(0x200000, 'x');
	argument.back() = '\0';

	pid_t pid = fork();
	assert_errno("fork", pid >= 0);
	if(!pid) {
		char *const args[] = {
			const_cast<char *>("/proc/self/exe"), argument.data(), nullptr
		};
		execve(args[0], args, nullptr);
		int error = errno;
		_exit(error == E2BIG ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	int status = 0;
	while(waitpid(pid, &status, 0) == -1) {
		if(errno == EINTR)
			continue;
		assert_errno("waitpid", false);
	}
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}))
