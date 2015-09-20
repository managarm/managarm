
#include <stdlib.h>
#include <stdio.h>

#include <spawn.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

#include <vector>

int main() {
	int fd = open("whatever", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	printf("Starting posix-init\n");

	std::vector<char *> argv;
	argv.push_back(const_cast<char *>("acpi"));
	argv.push_back(nullptr);

	char *envp[] = { nullptr };

	pid_t pid;
	if(posix_spawn(&pid, "zisa", nullptr, nullptr, argv.data(), envp)) {
		printf("posix_spawn() failed\n");
		abort();
	}
}

