#include <cassert>

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "testsuite.hpp"

namespace {
	void *offsetBy(void *ptr, ptrdiff_t n) {
		return reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(ptr)
					+ n);
	}

	sigjmp_buf restoreEnv;

	void signalHandler(int, siginfo_t *, void *) {
		siglongjmp(restoreEnv, 1);
	}

	bool ensureReadable(void *ptr) {
		if (sigsetjmp(restoreEnv, 1)) {
			return false;
		}

		(void)(*reinterpret_cast<volatile uint8_t *>(ptr));

		return true;
	}

	bool ensureWritable(void *ptr) {
		if (sigsetjmp(restoreEnv, 1)) {
			return false;
		}

		*reinterpret_cast<volatile uint8_t *>(ptr) = 0;

		return true;
	}

	bool ensureNotReadable(void *ptr) {
		if (sigsetjmp(restoreEnv, 1)) {
			return true;
		}

		(void)(*reinterpret_cast<volatile uint8_t *>(ptr));

		return false;
	}

	bool ensureNotWritable(void *ptr) {
		if (sigsetjmp(restoreEnv, 1)) {
			return true;
		}

		*reinterpret_cast<volatile uint8_t *>(ptr) = 0;

		return false;
	}

	template <typename Func>
	void runChecks(Func &&f) {
		pid_t pid = fork();
		assert_errno("fork", pid >= 0);

		struct sigaction sa, old_sa;
		sigemptyset(&sa.sa_mask);
		sa.sa_sigaction = signalHandler;
		sa.sa_flags = SA_SIGINFO;

		int ret = sigaction(SIGSEGV, &sa, &old_sa);
		assert_errno("sigaction", ret != -1);

		if (pid == 0) {
			f();
			exit(0);
		} else {
			int status = 0;
			while (waitpid(pid, &status, 0) == -1) {
				if (errno == EINTR) continue;
				assert_errno("waitpid", false);
			}

			if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
				fprintf(stderr, "Test failed on subprocess!\n");
				abort();
			}

			f();
		}

		ret = sigaction(SIGSEGV, &old_sa, nullptr);
		assert_errno("sigaction", ret != -1);
	}

	const size_t pageSize = sysconf(_SC_PAGESIZE);
} // namespace anonymous

DEFINE_TEST(mmap_fixed_replace_middle, ([] {
	void *mem = mmap(nullptr, pageSize * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	void *newPtr = mmap(offsetBy(mem, pageSize), pageSize, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
	assert_errno("mmap", newPtr != MAP_FAILED);
	assert(newPtr == offsetBy(mem, pageSize));

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureWritable(offsetBy(mem, pageSize * 2)));
	});

	int ret = munmap(mem, pageSize * 3);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureNotReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureNotWritable(offsetBy(mem, pageSize * 2)));
	});
}))

DEFINE_TEST(mmap_fixed_replace_left, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	void *newPtr = mmap(mem, pageSize, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
	assert_errno("mmap", newPtr != MAP_FAILED);
	assert(newPtr == mem);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureWritable(offsetBy(mem, pageSize)));
	});

	int ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_fixed_replace_right, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	void *newPtr = mmap(offsetBy(mem, pageSize), pageSize, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
	assert_errno("mmap", newPtr != MAP_FAILED);
	assert(newPtr == offsetBy(mem, pageSize));

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});

	int ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_partial_protect_middle, ([] {
	void *mem = mmap(nullptr, pageSize * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = mprotect(offsetBy(mem, pageSize), pageSize, PROT_READ);
	assert_errno("mprotect", ret != -1);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureWritable(offsetBy(mem, pageSize * 2)));
	});

	ret = munmap(mem, pageSize * 3);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureNotReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureNotWritable(offsetBy(mem, pageSize * 2)));
	});
}))

DEFINE_TEST(mmap_partial_protect_left, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = mprotect(mem, pageSize, PROT_READ);
	assert_errno("mprotect", ret != -1);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureWritable(offsetBy(mem, pageSize)));
	});

	ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_partial_protect_right, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = mprotect(offsetBy(mem, pageSize), pageSize, PROT_READ);
	assert_errno("mprotect", ret != -1);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});

	ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_partial_unmap_middle, ([] {
	void *mem = mmap(nullptr, pageSize * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = munmap(offsetBy(mem, pageSize), pageSize);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureWritable(offsetBy(mem, pageSize * 2)));
	});

	ret = munmap(mem, pageSize * 3);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));

		assert(ensureNotReadable(offsetBy(mem, pageSize * 2)));
		assert(ensureNotWritable(offsetBy(mem, pageSize * 2)));
	});
}))

DEFINE_TEST(mmap_partial_unmap_left, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = munmap(mem, pageSize);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureReadable(offsetBy(mem, pageSize)));
		assert(ensureWritable(offsetBy(mem, pageSize)));
	});

	ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_partial_unmap_right, ([] {
	void *mem = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = munmap(offsetBy(mem, pageSize), pageSize);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureReadable(mem));
		assert(ensureWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});

	ret = munmap(mem, pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));

		assert(ensureNotReadable(offsetBy(mem, pageSize)));
		assert(ensureNotWritable(offsetBy(mem, pageSize)));
	});
}))

DEFINE_TEST(mmap_unmap_range_before_first, ([] {
	void *mem = mmap(reinterpret_cast<void *>(0x100000 + pageSize * 2), pageSize,
			PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	assert_errno("mmap", mem != MAP_FAILED);

	int ret = munmap(reinterpret_cast<void *>(0x100000 + pageSize), pageSize * 2);
	assert_errno("munmap", ret != -1);

	runChecks([&] {
		assert(ensureNotReadable(mem));
		assert(ensureNotWritable(mem));
	});
}))
