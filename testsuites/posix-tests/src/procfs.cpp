#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(procfs_status_after_wait, ([] {
	pid_t pid = fork();
	assert(pid >= 0);

	if(!pid) {
		_exit(0);
	}

	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	int fd = open(path, O_RDONLY);
	assert(fd >= 0);

	char task_path[64];
	snprintf(task_path, sizeof(task_path), "/proc/%d/task/%d/status", pid, pid);
	int task_fd = open(task_path, O_RDONLY);
	assert(task_fd >= 0);

	siginfo_t dummy;
	int ret = waitid(P_PID, pid, &dummy, WEXITED | WNOWAIT);
	assert(ret == 0);

	char buf[64];
	lseek(fd, 0, SEEK_SET);
	ret = read(fd, buf, sizeof(buf) - 1);
	assert(ret > 0);
	lseek(fd, 0, SEEK_SET);

	std::ifstream pid_stream{path};

	bool found_zombie_state = false;
	for (std::string line; std::getline(pid_stream, line); ) {
		if (line.starts_with("State:")) {
			found_zombie_state = true;
			assert(line == "State:\tZ (zombie)");
			break;
		}
	}
	assert(found_zombie_state);

	// Same check for the task status file.
	lseek(task_fd, 0, SEEK_SET);
	ret = read(task_fd, buf, sizeof(buf) - 1);
	assert(ret > 0);
	lseek(task_fd, 0, SEEK_SET);

	std::ifstream tid_stream{task_path};
	found_zombie_state = false;
	for (std::string line; std::getline(tid_stream, line); ) {
		if (line.starts_with("State:")) {
			found_zombie_state = true;
			assert(line == "State:\tZ (zombie)");
			break;
		}
	}
	assert(found_zombie_state);

	int status;
	ret = waitpid(pid, &status, 0);
	assert(ret == pid);

	ret = read(fd, buf, sizeof(buf));
	assert(ret == -1);
	assert(errno == ESRCH);

	ret = read(task_fd, buf, sizeof(buf));
	assert(ret == -1);
	assert(errno == ESRCH);

	close(fd);
	close(task_fd);

	fd = open(path, O_RDONLY);
	assert(fd == -1);
	assert(errno == ENOENT);

	task_fd = open(task_path, O_RDONLY);
	assert(task_fd == -1);
	assert(errno == ENOENT);
}))

static void *thread_main(void *) {
	sleep(1);

	char task_path[64];
	snprintf(task_path, sizeof(task_path), "/proc/%d/task", getpid());
	std::filesystem::path task_dir{task_path};
	assert(std::filesystem::exists(task_dir));
	assert(std::filesystem::is_directory(task_dir));
	fprintf(stderr, "\t%s exists\n", task_path);

	snprintf(task_path, sizeof(task_path), "/proc/%d/task/%d", getpid(), getpid());
	std::filesystem::path main_thread_path{task_path};
	assert(std::filesystem::exists(main_thread_path));
	assert(std::filesystem::is_directory(main_thread_path));
	fprintf(stderr, "\t%s exists\n", task_path);

	snprintf(task_path, sizeof(task_path), "/proc/%d/task/%d", getpid(), gettid());
	std::filesystem::path thread_path{task_path};
	assert(std::filesystem::exists(thread_path));
	assert(std::filesystem::is_directory(thread_path));
	fprintf(stderr, "\t%s exists\n", task_path);

	pthread_exit(nullptr);
}

DEFINE_TEST(procfs_multithread_zombie, ([] {
	pid_t pid = fork();
	assert(pid >= 0);

	if(!pid) {
		pthread_t thread;
		int ret = pthread_create(&thread, nullptr, &thread_main, nullptr);
		assert(ret == 0);

		pthread_exit(nullptr);
	}

	siginfo_t dummy;
	int ret = waitid(P_PID, pid, &dummy, WEXITED | WNOWAIT);
	assert(ret == 0);

	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	std::ifstream stream{path};
	assert(stream.good());

	bool found_zombie_state = false;
	for (std::string line; std::getline(stream, line); ) {
		if (line.starts_with("State:")) {
			found_zombie_state = true;
			assert(line == "State:\tZ (zombie)");
			break;
		}
	}
	assert(found_zombie_state);

	char task_path[64];
	snprintf(task_path, sizeof(task_path), "/proc/%d/task/%d/status", pid, pid);
	std::ifstream task_stream{task_path};
	assert(task_stream.good());

	found_zombie_state = false;
	for (std::string line; std::getline(task_stream, line); ) {
		if (line.starts_with("State:")) {
			found_zombie_state = true;
			assert(line == "State:\tZ (zombie)");
			break;
		}
	}
	assert(found_zombie_state);

	int status;
	ret = waitpid(pid, &status, 0);
	assert(ret == pid);
}))
