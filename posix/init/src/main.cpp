
#include <assert.h>
#include <fcntl.h>
//#include <linux/input.h>
#include <netinet/in.h> // FIXME: for testing
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/socket.h> // FIXME: for testing
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <vector>

//FIXME:
#include <string.h>
#include <hel.h>
#include <hel-syscalls.h>

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	printf("Starting posix-init\n");

	std::vector<char *> args;
	args.push_back(const_cast<char *>("acpi"));
	args.push_back(nullptr);

	char *envp[] = { nullptr };

/*
	auto uhci = fork();
	if(!uhci) {
		execve("/initrd/uhci", args.data(), envp);
	}else assert(uhci != -1);
*/

	auto ehci = fork();
	if(!ehci) {
		execve("/initrd/ehci", args.data(), envp);
	}else assert(ehci != -1);
/*
	auto storage = fork();
	if(!storage) {
		execve("/initrd/storage", args.data(), envp);
	}else assert(storage != -1);
*/
/*
	auto kbd = fork();
	if(!kbd) {
		execve("/initrd/kbd", args.data(), envp);
	}else assert(kbd != -1);

	// Spin until /dev/event0 becomes available.
	while(access("/dev/event0", F_OK)) {
		assert(errno == ENOENT);
		for(int i = 0; i < 100; i++)
			sched_yield();
	}

	// dump input events
	const char *path = "/dev/event1";
	int file = open(path, 0);
	assert(!(file == -1));

	while(true) {
		input_event event;
		size_t read_bytes = read(file, &event, sizeof(input_event));
		assert(!(read_bytes == -1));
		
		printf("type: %i\n", event.type);
		printf("code: %i\n", event.code);
		printf("val: %i\n", event.value);
		printf("-----------\n");
	}
*/
/*
	// UART
	auto uart = fork();
	if(!uart) {
		execve("/initrd/uart", args.data(), envp);
	}else assert(uart != -1);
	
	// Spin until /dev/ttyS0 becomes available.
	while(access("/dev/ttyS0", F_OK)) {
		assert(errno == ENOENT);
		for(int i = 0; i < 100; i++)
			sched_yield();
	}
	
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	int tty = open("/dev/ttyS0", O_WRONLY);
	dup2(tty, STDOUT_FILENO);
	dup2(tty, STDERR_FILENO);
	
	std::cout << "........." << std::endl;

	while(true) {
		char buffer[1];
		auto read_sz = read(STDOUT_FILENO, buffer, 1);
		assert(read_sz > 0);
		std::cout << buffer[0] << std::endl;
	}
*/
/*
	auto gfx_intel = fork();
	if(!gfx_intel) {
		execve("/initrd/gfx_intel", args.data(), envp);
	}else assert(gfx_intel != -1);
*/

/*
	auto virtio = fork();
	if(!virtio) {
//		execve("/initrd/ata", args.data(), envp);
		execve("/initrd/virtio-block", args.data(), envp);
	}else assert(virtio != -1);
*/
/*	
	// Spin until /dev/sda0 becomes available.
	while(access("/dev/sda0", F_OK)) {
		assert(errno == ENOENT);
		sleep(1);
	}

	printf("Mounting /dev/sda0\n");
	if(mount("/dev/sda0", "/realfs", "ext2", 0, "")) {
		printf("Mount failed!\n");
	}else{
		printf("Mount success!\n");
	}
*/
/*
	auto vga_terminal = fork();
	if(!vga_terminal) {
		execve("/realfs/usr/bin/vga_terminal", args.data(), envp);
	}else assert(vga_terminal != -1);
*/

/*	
	// TODO: this is a very ugly hack to wait until the fs is ready
	for(int i = 0; i < 10000; i++)
		sched_yield();

	printf("Second fork, here we go!\n");

	pid_t terminal_child = fork();
	assert(terminal_child != -1);
	if(!terminal_child) {
//		execve("/usr/bin/kbd", args.data(), envp);
		execve("/usr/bin/uhci", args.data(), envp);
//		execve("/usr/bin/virtio-net", args.data(), envp);
//		execve("/usr/bin/bochs_vga", args.data(), envp);
//		execve("/usr/bin/zisa", args.data(), envp);
	}
*/	
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
}

