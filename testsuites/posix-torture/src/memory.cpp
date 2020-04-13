#include <cassert>
#include <sys/mman.h>

#include "testsuite.hpp"

DEFINE_TEST(map_unmap_anonymous, ([] {
	void *window = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(window != MAP_FAILED);
	munmap(window, 0x1000);
}))
