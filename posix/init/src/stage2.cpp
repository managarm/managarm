
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>

#include <libudev.h>

// This second stages assumes that:
// - stdin, stdout and stderr are already set up correctly
// - the real file system is mounted to /

int main() {
	std::cout << "init: Entering stage2" << std::endl;

	auto upload = [] (const char *name) {
		auto svrctl = fork();
		if(!svrctl) {
			execl("/usr/bin/runsvr", "/usr/bin/runsvr", "upload", name, nullptr);
		}else assert(svrctl != -1);

		// TODO: Ensure the status is termination.
		waitpid(svrctl, nullptr, 0);
	};

	upload("/usr/bin/gfx_bochs");
	upload("/usr/bin/gfx_plainfb");
	upload("/usr/bin/gfx_virtio");
	upload("/usr/bin/gfx_vmware");
	upload("/usr/bin/ps2-hid");
	upload("/usr/bin/hid");
	upload("/usr/lib/libevbackend.so");
	upload("/usr/lib/libdrm_core.so");

	// Start udev which loads the remaining drivers.
	auto udev = fork();
	if(!udev) {
		execl("/usr/sbin/udevd", "udevd", nullptr);
//		execl("/usr/sbin/udevd", "udevd", "--debug", nullptr);
	}else assert(udev != -1);

	while(access("/run/udev/rules.d", F_OK)) { // TODO: Use some other file to wait on?
		assert(errno == ENOENT);
		sleep(1);
	}

	// Create a udev monitor to watch for new devices.
	// Do this before we run 'udevadm trigger' so that the monitor will see all devices.
	auto init_udev = udev_new();
	if(!init_udev) {
		std::cerr << "\e[31m" "init: udev_new() failed" "\e[39m" << std::endl;
		abort();
	}

	auto init_udev_mon = udev_monitor_new_from_netlink(init_udev, "udev");
	if(!init_udev_mon) {
		std::cerr << "\e[31m" "init: udev_monitor_new_from_netlink() failed" "\e[39m" << std::endl;
		abort();
	}
	if(udev_monitor_enable_receiving(init_udev_mon) < 0) {
		std::cerr << "\e[31m" "init: udev_monitor_new_from_netlink() failed" "\e[39m" << std::endl;
		abort();
	}

	// Now run 'udevadm trigger' to make sure that udev initializes every device.
	std::cout << "init: Running udev-trigger" << std::endl;
	auto udev_trigger_devs = fork();
	if(!udev_trigger_devs) {
		execl("/usr/bin/udevadm", "udevadm", "trigger", "--action=add", nullptr);
	}else assert(udev_trigger_devs != -1);

	waitpid(udev_trigger_devs, nullptr, 0);

	std::cout << "init: Running udev-settle" << std::endl;
	auto udev_settle = fork();
	if(!udev_settle) {
		execl("/usr/bin/udevadm", "udevadm", "settle", nullptr);
	}else assert(udev_settle != -1);

	waitpid(udev_settle, nullptr, 0);

	std::cout << "init: udev initialization is done" << std::endl;

	// Start some drivers that are not integrated into udev rules yet.
	auto input_ps2 = fork();
	if(!input_ps2) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "runsvr", "/usr/bin/ps2-hid", nullptr);
	}else assert(input_ps2 != -1);

	auto input_hid = fork();
	if(!input_hid) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "runsvr", "/usr/bin/hid", nullptr);
	}else assert(input_hid != -1);

	// Wait until we have the devices required for weston.
	bool have_drm = false;
	bool have_kbd = false;
	bool have_mouse = false;

	std::cout << "init: Waiting for devices to show up" << std::endl;
	while(!have_drm || !have_kbd || !have_mouse) {
		auto dev = udev_monitor_receive_device(init_udev_mon);
		if(!dev) {
			std::cerr << "\e[31m" "init: udev_monitor_receive_device() failed"
					"\e[39m" << std::endl;
			abort();
		}
		auto syspath = udev_device_get_syspath(dev);
		assert(syspath);
		auto subsystem = udev_device_get_subsystem(dev);
		assert(subsystem);

		if(!strcmp(subsystem, "drm")) {
			std::cout << "init: Found DRM device " << syspath << std::endl;
			have_drm = true;
		}
		if(!strcmp(subsystem, "input")) {
			if(udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD")) {
				std::cout << "init: Found keyboard " << syspath << std::endl;
				have_kbd = true;
			}
			if(udev_device_get_property_value(dev, "ID_INPUT_MOUSE")) {
				std::cout << "init: Found mouse " << syspath << std::endl;
				have_mouse = true;
			}
		}

		udev_device_unref(dev);
	}

	// Finally, launch into Weston.
	auto modeset = fork();
	if(!modeset) {
//		putenv("MLIBC_DEBUG_MALLOC=1");
		putenv("PATH=/usr/local/bin:/usr/bin:/bin");
		putenv("XDG_RUNTIME_DIR=/run");
		putenv("MESA_GLSL_CACHE_DISABLE=1");
//		putenv("MESA_DEBUG=1");

		//execl("/usr/bin/weston", "weston", nullptr);
		//execl("/usr/bin/weston", "weston", "--use-pixman", nullptr);
		execl("/usr/bin/kmscon", "kmscon", nullptr);
		//execl("/usr/bin/kmscon", "kmscon", "--debug", "--verbose", nullptr);
		//execl("/usr/bin/kmscube", "kmscube", nullptr);
	}else assert(modeset != -1);

	while(true)
		sleep(60);
}

