#include <assert.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "testsuite.hpp"

DEFINE_TEST(exec_rejects_writable_load_with_filesz_larger_than_memsz, ([] {
#if defined(__linux__)
	// The ELF gABI requires p_filesz <= p_memsz for PT_LOAD. Linux checks
	// this only after begin_new_exec(), so the malformed image terminates
	// with SIGSEGV instead of returning ENOEXEC. Test this on Managarm,
	// whose loader rejects the image before replacing the old process image.
	skip_test("Linux rejects malformed ELF after replacing the process image");
#endif
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
	constexpr size_t fileSize = segmentOffset + 0x1001;
	std::vector<char> image(fileSize);

	Elf64_Ehdr ehdr{};
	ehdr.e_ident[EI_MAG0] = ELFMAG0;
	ehdr.e_ident[EI_MAG1] = ELFMAG1;
	ehdr.e_ident[EI_MAG2] = ELFMAG2;
	ehdr.e_ident[EI_MAG3] = ELFMAG3;
	ehdr.e_ident[EI_CLASS] = ELFCLASS64;
	ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_type = ET_EXEC;
	ehdr.e_machine = machine;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_phoff = sizeof(Elf64_Ehdr);
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = 1;

	Elf64_Phdr phdr{};
	phdr.p_type = PT_LOAD;
	phdr.p_flags = PF_R | PF_W;
	phdr.p_offset = segmentOffset;
	phdr.p_vaddr = 0;
	phdr.p_filesz = 0x1001;
	phdr.p_memsz = 1;
	phdr.p_align = 0x1000;

	memcpy(image.data(), &ehdr, sizeof(ehdr));
	memcpy(image.data() + ehdr.e_phoff, &phdr, sizeof(phdr));

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
		_exit(error == ENOEXEC ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	int status = 0;
	while(waitpid(pid, &status, 0) == -1) {
		if(errno == EINTR)
			continue;
		assert_errno("waitpid", false);
	}
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == EXIT_SUCCESS);
	assert(unlink(path) == 0);
}))
