#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include "testsuite.hpp"

namespace {

sigjmp_buf null_access_jump_buffer;
volatile sig_atomic_t null_access_signal_caught = 0;
volatile int null_access_code = 0;
volatile int null_access_signo = 0;
void * volatile null_access_addr = nullptr;

void null_access_handler(int sig, siginfo_t *si, void *unused) {
	(void) sig;
	(void) unused;

	null_access_signo = si->si_signo;
	null_access_code = si->si_code;
	null_access_addr = si->si_addr;
	null_access_signal_caught = 1;

	siglongjmp(null_access_jump_buffer, 1);
}

} // namespace

DEFINE_TEST(segfault_null_access, ([] {
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = null_access_handler;
	sigemptyset(&sa.sa_mask);

	int ret = sigaction(SIGSEGV, &sa, nullptr);
	assert(!ret);

	volatile int *bad_ptr = nullptr;
	asm volatile ("" : "+r"(bad_ptr));

	if (sigsetjmp(null_access_jump_buffer, 1) == 0) {
		*bad_ptr = 42;
	} else {
		assert(null_access_signal_caught);
		assert(null_access_signo == SIGSEGV);
		assert(null_access_addr == bad_ptr);
		assert(null_access_code == SEGV_MAPERR);

		signal(SIGSEGV, SIG_DFL);
	}
}))

namespace {

sigjmp_buf jump_buffer;
volatile sig_atomic_t signal_caught = 0;
volatile int received_si_code = 0;
volatile int received_si_signo = 0;
void * volatile received_si_addr = nullptr;

void segv_handler(int sig, siginfo_t *si, void *unused) {
	(void)sig;
	(void)unused;

	received_si_signo = si->si_signo;
	received_si_code = si->si_code;
	received_si_addr = si->si_addr;
	signal_caught = 1;

	siglongjmp(jump_buffer, 1);
}

} // namespace

DEFINE_TEST(segfault_write_to_readonly, ([] {
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_flags = SA_SIGINFO; // IMPORTANT: Required to get siginfo_t
	sa.sa_sigaction = segv_handler;
	sigemptyset(&sa.sa_mask);

	int ret = sigaction(SIGSEGV, &sa, nullptr);
	assert(!ret);

	void *ro_mem = mmap(nullptr, 0x4000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(ro_mem != MAP_FAILED);

	if (sigsetjmp(jump_buffer, 1) == 0) {
		*(int *)ro_mem = 99;
	} else {
		assert(signal_caught);
		assert(received_si_signo == SIGSEGV);
		assert(received_si_addr == ro_mem);
		assert(received_si_code == SEGV_ACCERR);

		signal(SIGSEGV, SIG_DFL);
	}

	// Cleanup
	munmap(ro_mem, 0x4000);
}))
