#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(shm_basic, ([] {
	int shmid = shmget(IPC_PRIVATE, 0x1000, IPC_CREAT | 0600);
	assert(shmid != -1);

	void *addr = shmat(shmid, nullptr, 0);
	assert(addr != (void *)-1);
	memset(addr, 0x42, 0x1000);

	int ret = shmdt(addr);
	assert(ret == 0);

	ret = shmctl(shmid, IPC_RMID, nullptr);
	assert(ret == 0);
}))

DEFINE_TEST(shm_stat, ([] {
	constexpr size_t size = 0x2000;
	constexpr int perms = 0640;

	int shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | perms);
	assert(shmid != -1);

	struct shmid_ds ds;
	int ret = shmctl(shmid, IPC_STAT, &ds);
	assert(ret == 0);
	assert(ds.shm_segsz == size);
	assert((ds.shm_perm.mode & 0777) == perms);

	ret = shmctl(shmid, IPC_RMID, nullptr);
	assert(ret == 0);
}))

DEFINE_TEST(shm_fork, ([] {
	constexpr uint32_t magic = 0xDEADBEEF;

	int shmid = shmget(IPC_PRIVATE, 0x1000, IPC_CREAT | 0600);
	assert(shmid != -1);

	uint32_t *data = static_cast<uint32_t *>(shmat(shmid, nullptr, 0));
	assert(data != (void *)-1);
	*data = 0;

	pid_t pid = fork();
	assert(pid != -1);

	if (!pid) {
		// Child: write magic value to shared memory.
		*data = magic;
		_exit(0);
	}

	// Parent: wait for child.
	int status;
	pid_t waited = waitpid(pid, &status, 0);
	assert(waited == pid);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);

	// Verify the child's write is visible.
	assert(*data == magic);

	int ret = shmdt(data);
	assert(ret == 0);

	ret = shmctl(shmid, IPC_RMID, nullptr);
	assert(ret == 0);
}))
