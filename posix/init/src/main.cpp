
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
	if(!child) {
//		execve("kbd", args.data(), envp);
//		execve("bochs_vga", args.data(), envp);
//		execve("vga_terminal", args.data(), envp);
		execve("ata", args.data(), envp);
//		execve("zisa", args.data(), envp);
	}

	printf("Second fork, here we go!\n");

	pid_t child2 = fork();
	assert(child2 != -1);
	if(!child2) {
		execve("vga_terminal", args.data(), envp);
	}
}

