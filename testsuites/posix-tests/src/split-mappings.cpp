#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

constexpr uint64_t MAGIC_VALUE = 0xDEADBEEFCAFEBABE;

DEFINE_TEST(split_cow_mappings_fork, ([] {
	volatile void *map_base = mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (map_base == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	auto base_ptr = static_cast<volatile char *>(map_base);
	auto second_page_ptr = base_ptr + 0x1000;

	// touch the second page by writing a magic value
	*reinterpret_cast<volatile uint64_t *>(second_page_ptr) = MAGIC_VALUE;

	// split the mapping by unmapping the first page
	if (munmap((void *)map_base, 0x1000) == -1) {
		perror("munmap");
		exit(1);
	}

	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(1);
	}

	if (pid == 0) {
		uint64_t read_value = *reinterpret_cast<volatile uint64_t *>(second_page_ptr);

		if (read_value == MAGIC_VALUE)
			exit(0);
		else
			exit(1);
	} else {
		uint64_t read_value = *reinterpret_cast<volatile uint64_t *>(second_page_ptr);

		assert(read_value == MAGIC_VALUE);

		int status;
		waitpid(pid, &status, 0);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	munmap((void *) second_page_ptr, 0x1000);
}))
