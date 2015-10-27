
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <unordered_map>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>
#include <thor.h>

helx::EventHub eventHub = helx::EventHub::create();

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

void thread1() {
	helLog("a", 1);
	while(true) {
//		helLog("a", 1);
	}
}

void thread2() {
	helLog("b", 1);
	while(true) {
//		helLog("b", 1);
	}
}

uint8_t stack1[4096];
uint8_t stack2[4096];

#include <unistd.h>
#include <sys/helfd.h>

extern "C" void testSignal() {
	printf("In signal\n");
}

asm ( ".section .text\n"
	".global signalEntry\n"
	"signalEntry:\n"
	"\tcall testSignal\n"
	"\tmov $40, %rdi\n"
	"\tsyscall\n" );

extern "C" void signalEntry();

int main() {
	printf("Hello world\n");

	HEL_CHECK(helYield());

/*	HelHandle handle;
	HEL_CHECK(helCreateSignal((void *)&signalEntry, &handle));
	HEL_CHECK(helRaiseSignal(handle));
	
	printf("After signal\n");

/*	int fd = open("/dev/hw", O_RDONLY);
	assert(fd != -1);

	HelHandle handle;
	int clone_res = helfd_clone(fd, &handle);
	assert(clone_res == 0);

	HelThreadState state;
	HelHandle handle1, handle2;

	state.rip = (uint64_t)&thread1;
	state.rsp = (uint64_t)(stack1 + 4096);
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle, &state, 0, &handle1));
	
	state.rip = (uint64_t)&thread2;
	state.rsp = (uint64_t)(stack2 + 4096);
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle, &state, 0, &handle1));*/

	HEL_CHECK(helControlKernel(kThorSubDebug, kThorIfDebugMemory, nullptr, nullptr));
}

