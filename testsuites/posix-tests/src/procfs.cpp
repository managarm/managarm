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

DEFINE_TEST(procfs_cpuinfo, ([] {
	std::ifstream stream{"/proc/cpuinfo"};
	assert(stream.good());

	long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	assert(num_cpus > 0);

	long num_records = 0;
	long num_model_names = 0;
	long num_steppings = 0;
	long num_microcodes = 0;
	long num_frequencies = 0;
	long num_cpuid_levels = 0;
	long num_address_sizes = 0;
	long num_flags = 0;
	long num_bugs = 0;
	long num_bogomips = 0;
#if defined(__x86_64__)
	long num_apicids = 0;
	long num_clflush_sizes = 0;
	long num_cache_alignments = 0;
	long num_fpus = 0;
	long num_fpu_exceptions = 0;
	long num_write_protects = 0;
#endif
#if defined(__aarch64__)
	long num_features = 0;
	long num_cpu_implementers = 0;
	long num_cpu_architectures = 0;
	long num_cpu_variants = 0;
	long num_cpu_parts = 0;
	long num_cpu_revisions = 0;
#elif defined(__riscv)
	long num_harts = 0;
	long num_isas = 0;
	long num_mmus = 0;
#endif
	for(std::string line; std::getline(stream, line); ) {
		if(line.starts_with("processor\t:"))
			++num_records;
		if(line.starts_with("model name\t:")) {
			assert(line.size() > sizeof("model name\t: ") - 1);
			++num_model_names;
		}
		if(line.starts_with("stepping\t:"))
			++num_steppings;
		if(line.starts_with("microcode\t:"))
			++num_microcodes;
		if(line.starts_with("cpu MHz\t\t:"))
			++num_frequencies;
		if(line.starts_with("cpuid level\t:"))
			++num_cpuid_levels;
		if(line.starts_with("address sizes\t:"))
			++num_address_sizes;
		if(line.starts_with("flags\t\t:") && line.size() > sizeof("flags\t\t: ") - 1)
			++num_flags;
		if(line.starts_with("bugs\t\t:"))
			++num_bugs;
		if((line.starts_with("bogomips\t:") || line.starts_with("BogoMIPS\t:"))
				&& line.size() > sizeof("bogomips\t: ") - 1)
			++num_bogomips;
#if defined(__x86_64__)
		if(line.starts_with("apicid\t\t:"))
			++num_apicids;
		if(line.starts_with("clflush size\t:"))
			++num_clflush_sizes;
		if(line.starts_with("cache_alignment\t:"))
			++num_cache_alignments;
		if(line.starts_with("fpu\t\t:"))
			++num_fpus;
		if(line.starts_with("fpu_exception\t:"))
			++num_fpu_exceptions;
		if(line.starts_with("wp\t\t:"))
			++num_write_protects;
#endif
#if defined(__aarch64__)
		if(line.starts_with("Features\t:") && line.size() > sizeof("Features\t: ") - 1)
			++num_features;
		if(line.starts_with("CPU implementer\t:"))
			++num_cpu_implementers;
		if(line.starts_with("CPU architecture:"))
			++num_cpu_architectures;
		if(line.starts_with("CPU variant\t:"))
			++num_cpu_variants;
		if(line.starts_with("CPU part\t:"))
			++num_cpu_parts;
		if(line.starts_with("CPU revision\t:"))
			++num_cpu_revisions;
#elif defined(__riscv)
		if(line.starts_with("hart\t\t:") && line.size() > sizeof("hart\t\t: ") - 1)
			++num_harts;
		if(line.starts_with("isa\t\t:") && line.size() > sizeof("isa\t\t: ") - 1)
			++num_isas;
		if(line.starts_with("mmu\t\t:") && line.size() > sizeof("mmu\t\t: ") - 1)
			++num_mmus;
#endif
	}

	assert(num_records == num_cpus);
#if defined(__x86_64__)
	assert(num_model_names == num_cpus);
	assert(num_steppings == num_cpus);
	assert(num_microcodes == num_cpus);
	assert(num_frequencies == num_cpus);
	assert(num_cpuid_levels == num_cpus);
	assert(num_address_sizes == num_cpus);
	assert(num_flags == num_cpus);
	assert(num_bugs == num_cpus);
	assert(num_bogomips == num_cpus);
	assert(num_apicids == num_cpus);
	assert(num_clflush_sizes == num_cpus);
	assert(num_cache_alignments == num_cpus);
	assert(num_fpus == num_cpus);
	assert(num_fpu_exceptions == num_cpus);
	assert(num_write_protects == num_cpus);
#elif defined(__aarch64__)
	assert(num_bogomips == num_cpus);
	assert(num_features == num_cpus);
	assert(num_cpu_implementers == num_cpus);
	assert(num_cpu_architectures == num_cpus);
	assert(num_cpu_variants == num_cpus);
	assert(num_cpu_parts == num_cpus);
	assert(num_cpu_revisions == num_cpus);
#elif defined(__riscv)
	assert(num_harts == num_cpus);
	assert(num_isas == num_cpus);
	assert(num_mmus == num_cpus);
#endif
}))

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
