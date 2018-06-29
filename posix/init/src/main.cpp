
#include <assert.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	printf("Starting posix-init\n");

	// Start essential bus and storage drivers.
	auto ehci = fork();
	if(!ehci) {
		execl("/bin/runsvr", "runsvr", "/sbin/ehci", nullptr);
	}else assert(ehci != -1);

/*
	auto virtio = fork();
	if(!virtio) {
		execl("/bin/runsvr", "runsvr", "/sbin/virtio-block", nullptr);
	}else assert(virtio != -1);
*/

/*
	auto block_ata = fork();
	if(!block_ata) {
		execl("/bin/runsvr", "runsvr", "/sbin/block-ata", nullptr);
	}else assert(block_ata != -1);
*/

	auto block_usb = fork();
	if(!block_usb) {
		execl("/bin/runsvr", "runsvr", "/sbin/storage", nullptr);
	}else assert(block_usb != -1);

	// Spin until /dev/sda0 becomes available. Then mount the rootfs and prepare it.
	while(access("/dev/sda0", F_OK)) {
		std::cout << "Waiting for /dev/sda0" << std::endl;
		assert(errno == ENOENT);
		sleep(1);
	}

	// Hack: Start UHCI only after EHCI devices are ready.
	auto uhci = fork();
	if(!uhci) {
		execl("/bin/runsvr", "runsvr", "/sbin/uhci", nullptr);
	}else assert(uhci != -1);

	printf("init: Mounting /dev/sda0\n");
	if(mount("/dev/sda0", "/realfs", "ext2", 0, ""))
		throw std::runtime_error("mount() failed");

	if(mount("", "/realfs/sys", "sysfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/dev", "devtmpfs", 0, ""))
		throw std::runtime_error("mount() failed");
	if(mount("", "/realfs/run", "tmpfs", 0, ""))
		throw std::runtime_error("mount() failed");

	if(mkdir("/dev/pts", 0620))
		throw std::runtime_error("mkdir() failed");
	if(mount("", "/realfs/dev/pts", "devpts", 0, ""))
		throw std::runtime_error("mount() failed");

	if(chroot("/realfs"))
		throw std::runtime_error("chroot() failed");

	std::cout << "init: On /realfs" << std::endl;

	auto upload = [] (const char *name) {
		auto svrctl = fork();
		if(!svrctl) {
			execl("/usr/bin/runsvr", "upload", name, nullptr);
		}else assert(svrctl != -1);

		// TODO: Ensure the status is termination.
		waitpid(svrctl, nullptr, 0);
	};

	upload("/usr/bin/gfx_bochs");
	upload("/usr/bin/gfx_plainfb");
	upload("/usr/bin/gfx_virtio");
	upload("/usr/bin/ps2-hid");
	upload("/usr/bin/hid");
	upload("/usr/lib/libevbackend.so");
	upload("/usr/lib/libdrm_core.so");

/*
	auto gfx_virtio = fork();
	if(!gfx_virtio) {
		execl("/usr/bin/runsvr", "runsvr", "/usr/bin/gfx_virtio", nullptr);
	}else assert(gfx_virtio != -1);
*/

/*	
	auto gfx_bochs = fork();
	if(!gfx_bochs) {
		execl("/usr/bin/runsvr", "runsvr", "/usr/bin/gfx_bochs", nullptr);
	}else assert(gfx_bochs != -1);
*/

	auto gfx_plainfb = fork();
	if(!gfx_plainfb) {
		execl("/usr/bin/runsvr", "runsvr", "/usr/bin/gfx_plainfb", nullptr);
	}else assert(gfx_plainfb != -1);

	while(access("/dev/dri/card0", F_OK)) {
		assert(errno == ENOENT);
		sleep(1);
	}

/*
	while(access("/dev/input/event0", F_OK)) {
		assert(errno == ENOENT);
		sleep(1);
	}
*/

	auto udev = fork();
	if(!udev) {
		execl("/usr/sbin/udevd", "udevd", nullptr);
		//execl("/usr/sbin/udevd", "udevd", "--debug", nullptr);
	}else assert(udev != -1);

	while(access("/run/udev/rules.d", F_OK)) { // TODO: Use some other file to wait on?
		assert(errno == ENOENT);
		sleep(1);
	}

/*
	auto input_ps2 = fork();
	if(!input_ps2) {
		execl("/usr/bin/runsvr", "runsvr", "/usr/bin/ps2-hid", nullptr);
	}else assert(input_ps2 != -1);
*/

	auto input_hid = fork();
	if(!input_hid) {
		execl("/usr/bin/runsvr", "runsvr", "/usr/bin/hid", nullptr);
	}else assert(input_hid != -1);

	sleep(3);

/*
	auto udev_trigger_devs = fork();
	if(!udev_trigger_devs) {
		execl("/usr/bin/udevadm", "udevadm", "trigger", "--action=add", nullptr);
	}else assert(udev_trigger_devs != -1);
*/

	auto modeset = fork();
	if(!modeset) {
//		putenv("MLIBC_DEBUG_MALLOC=1");
		putenv("XDG_RUNTIME_DIR=/run");
		putenv("MESA_GLSL_CACHE_DISABLE=1");
//		putenv("MESA_DEBUG=1");
//		putenv("TGSI_PRINT_SANITY=1");
//		putenv("SOFTPIPE_NO_RAST=1");
//		putenv("SOFTPIPE_DUMP_FS=1");

		//execve("/root/unixsock", args.data(), env.data());
		//execve("/root/test-libudev", args.data(), env.data());
//		execl("/usr/bin/kmscube", "kmscube", nullptr);
		//execve("/root/modeset-render", args.data(), env.data());
		//execve("/root/modeset-double-buffered", args.data(), env.data());
		//execl("/usr/bin/weston", "weston", "--use-pixman", nullptr);
		execl("/usr/bin/weston", "weston", nullptr);
	}else assert(modeset != -1);

	while(true)
		sleep(60);
}

