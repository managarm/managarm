
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h> // FIXME: for testing
#include <netinet/in.h> // FIXME: for testing
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
//		execve("/initrd/ata", args.data(), envp);
		execve("/initrd/virtio-block", args.data(), envp);
	}
	
	// TODO: this is a very ugly hack to wait until the fs is ready
	for(int i = 0; i < 10000; i++)
		sched_yield();

	printf("Second fork, here we go!\n");

	pid_t terminal_child = fork();
	assert(terminal_child != -1);
	if(!terminal_child) {
		execve("/usr/bin/kbd", args.data(), envp);
//		execve("/usr/bin/virtio-net", args.data(), envp);
//		execve("/usr/bin/bochs_vga", args.data(), envp);
//		execve("/usr/bin/zisa", args.data(), envp);
	}
	
	// TODO: this is a very ugly hack to wait until the fs is ready
/*	for(int i = 0; i < 10000; i++)
		sched_yield();
	
	printf("Testing network API!\n");
	
	int socket = open("/dev/network/ip+udp", O_RDWR);
	
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = 7;
	address.sin_addr.s_addr = (10 << 24) | (85 << 16) | (1 << 8) | 1;
	connect(socket, (struct sockaddr *)&address, sizeof(struct sockaddr_in));
	write(socket, "hello", 5);*/

	// FIXME: work around kernel bug
	while(true)
		sched_yield();
}

