#include <assert.h>
#include <atomic>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

namespace {
	std::atomic<int> signalFlag = 0;
} // namespace anonymous

constexpr uint64_t magic_expected = 0xDEADBEEFCAFEBABE;

#if !defined(__linux__)
DEFINE_TEST(signal_save_simd, ([] {
	int ret;

#if defined (__x86_64__)
	asm volatile ("movd %0, %%xmm15" : : "r"(magic_expected));
#elif defined (__aarch64__)
	asm volatile ("fmov d31, %0" : : "r"(magic_expected));
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif

	struct sigaction sa;
	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_flags = SA_RESETHAND;
	sa.sa_handler = [] (int) {
		signalFlag = 1;

#if defined (__x86_64__)
		asm volatile ("movd %0, %%xmm15" : : "r"(uint64_t(0x2BADBADBADBADBAD)));
#elif defined (__aarch64__)
		asm volatile ("fmov d31, xzr");
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif
	};

	ret = sigaction(SIGUSR1, &sa, nullptr);
	assert(ret == 0);

	ret = kill(getpid(), SIGUSR1);
	assert(ret == 0);

	assert(signalFlag == 1);

	uint64_t magic = 0;

#if defined (__x86_64__)
	asm volatile ("movd %%xmm15, %0" : "=r"(magic));
#elif defined (__aarch64__)
	asm volatile ("fmov %0, d31" : "=r"(magic));
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif

	assert(magic == magic_expected);
}))

#endif

namespace {
	std::atomic<int> nodefer_signal_flag = 0;
	std::atomic<int> nodefer_mask_is_set = 0;
} // namespace anonymous

DEFINE_TEST(signal_nodefer, ([] {
	struct sigaction sa;
	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_flags = SA_RESETHAND;
	sa.sa_handler = [] (int) {
		sigset_t set;
		int ret = sigprocmask(SIG_BLOCK, NULL, &set);
		assert(ret == 0);
		if(sigismember(&set, SIGUSR1))
			nodefer_mask_is_set = 1;

		nodefer_signal_flag = 1;
	};

	int ret = sigaction(SIGUSR1, &sa, nullptr);
	assert(ret == 0);

	nodefer_signal_flag = 0;
	nodefer_mask_is_set = 0;

	ret = kill(getpid(), SIGUSR1);
	assert(ret == 0);

	assert(nodefer_signal_flag == 1);
	assert(nodefer_mask_is_set == 1);

	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_flags = SA_NODEFER;
	sa.sa_handler = [] (int) {
		sigset_t set;
		int ret = sigprocmask(SIG_BLOCK, NULL, &set);
		assert(ret == 0);
		if(sigismember(&set, SIGUSR1))
			nodefer_mask_is_set = 1;

		nodefer_signal_flag = 1;
	};

	ret = sigaction(SIGUSR1, &sa, nullptr);
	assert(ret == 0);

	nodefer_signal_flag = 0;
	nodefer_mask_is_set = 0;

	ret = kill(getpid(), SIGUSR1);
	assert(ret == 0);

	assert(nodefer_signal_flag == 1);
	assert(nodefer_mask_is_set == 0);
}))

DEFINE_TEST(kill_null_signal, ([] {
	int ret = kill(1, 0);
	assert(!ret || errno == EPERM);

	pid_t pid = fork();
	assert(pid >= 0);

	if(!pid) {
		_exit(0);
	}

	int status;
	ret = waitpid(pid, &status, 0);
	assert(ret == pid);

	ret = kill(pid, 0);
	assert(ret == -1);
	assert(errno == ESRCH);
}))

DEFINE_TEST(sigchld_ignore, ([] {
	struct sigaction sa = {};
	sa.sa_handler = SIG_IGN;
	struct sigaction old = {};
	int ret = sigaction(SIGCHLD, &sa, &old);
	assert(!ret);

	pid_t pid = fork();
	assert(pid >= 0);

	if(!pid)
		_exit(0);

	int status;
	ret = waitpid(pid, &status, 0);
	assert(ret == -1);
	assert(errno == ECHILD);

	ret = sigaction(SIGCHLD, &old, nullptr);
	assert(!ret);

	sa = {};
	sa.sa_flags = SA_NOCLDWAIT;
	ret = sigaction(SIGCHLD, &sa, nullptr);
	assert(!ret);

	pid = fork();
	assert(pid >= 0);

	if(!pid)
		_exit(0);

	ret = waitpid(pid, &status, 0);
	assert(ret == -1);
	assert(errno == ECHILD);

	ret = sigaction(SIGCHLD, &old, nullptr);
	assert(!ret);

	sa = {};
	sa.sa_handler = SIG_DFL;
	ret = sigaction(SIGCHLD, &sa, nullptr);
	assert(!ret);

	pid = fork();
	assert(pid >= 0);

	if(!pid)
		_exit(0);

	ret = waitpid(pid, &status, 0);
	assert(ret == pid);

	ret = sigaction(SIGCHLD, &old, nullptr);
	assert(!ret);
}))
