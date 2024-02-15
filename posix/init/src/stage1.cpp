
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	std::cout <<"init: Entering first stage" << std::endl;

#if defined (__x86_64__)
	auto uart = fork();
	if(!uart) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/uart", nullptr);
	}else assert(uart != -1);
#endif

	// Start essential bus and storage drivers.
	auto ehci = fork();
	if(!ehci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/ehci", nullptr);
	}else assert(ehci != -1);

	auto xhci = fork();
	if(!xhci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/xhci", nullptr);
	}else assert(xhci != -1);

	auto virtio = fork();
	if(!virtio) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/virtio-block", nullptr);
	}else assert(virtio != -1);

#if defined (__x86_64__)
	auto block_ata = fork();
	if(!block_ata) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/block-ata", nullptr);
	}else assert(block_ata != -1);
#endif

	auto block_ahci = fork();
	if(!block_ahci) {
		execl("/bin/runsvr", "/bin/runsvr", "run", "/lib/block-ahci.bin", nullptr);
	}else assert(block_ahci != -1);

	auto block_nvme = fork();
	if(!block_nvme) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/block-nvme", nullptr);
	}else assert(block_nvme != -1);

	auto block_usb = fork();
	if(!block_usb) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/storage", nullptr);
	}else assert(block_usb != -1);

	// Spin until /dev/sda0 becomes available. Then mount the rootfs and prepare it.
	while(access("/dev/sda0", F_OK)) {
		assert(errno == ENOENT);
		std::cout << "Waiting for /dev/sda0" << std::endl;
		sleep(1);
	}

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	auto uhci = fork();
	if(!uhci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/uhci", nullptr);
	}else assert(uhci != -1);
#endif

	std::cout << "init: Mounting /dev/sda0" << std::endl;
	if(mount("/dev/sda0", "/realfs", "ext2", 0, ""))
		throw std::runtime_error("mount() failed");

	if(mount("", "/realfs/proc", "procfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/sys", "sysfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/dev", "devtmpfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/run", "tmpfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/tmp", "tmpfs", 0, ""))
		throw std::runtime_error("mount() failed");

	if(mkdir("/dev/pts", 0620))
		throw std::runtime_error("mkdir() failed");
	if(mount("", "/realfs/dev/pts", "devpts", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mkdir("/dev/shm", 1777)) // Seems to be the same as linux
		throw std::runtime_error("mkdir() failed");
	if(mount("", "/realfs/dev/shm", "tmpfs", 0, ""))
		throw std::runtime_error("mount() failed");

	if(chroot("/realfs"))
		throw std::runtime_error("chroot() failed");
	// Some programs, e.g. bash with its builtin getcwd() cannot deal with CWD outside of /.
	if(chdir("/"))
		throw std::runtime_error("chdir() failed");

	std::cout << "init: On /realfs" << std::endl;

	// /run needs to be 0700 or programs start complaining.
	if(chmod("/run", 0700))
		throw std::runtime_error("chmod() failed");

	execl("/usr/bin/init-stage2", "/usr/bin/init-stage2", nullptr);
	std::cout << "init: Failed to execve() second stage" << std::endl;
	abort();
}

