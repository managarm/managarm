
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <vector>

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	printf("Starting posix-init\n");

	std::vector<char *> args;
	args.push_back(const_cast<char *>("acpi"));
	args.push_back(nullptr);

	char *envp[] = { nullptr };

	pid_t child = fork();
	assert(child != -1);

	if(child == 0) {
		execve("zisa", args.data(), envp);
	}
}

