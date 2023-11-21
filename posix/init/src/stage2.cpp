#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iostream>

#include <libudev.h>

// This second stages assumes that:
// - stdin, stdout and stderr are already set up correctly
// - the real file system is mounted to /

int main() {
	std::cout << "init: Entering stage2" << std::endl;

	// We need a PATH for scripts run by xbps-reconfigure.
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);

	std::cout << "init: Running xbps-reconfigure" << std::endl;
	auto xbps_reconfigure = fork();
	if(!xbps_reconfigure) {
		execl("/usr/bin/xbps-reconfigure", "xbps-reconfigure", "-a", "-v", nullptr);
	}else assert(xbps_reconfigure != -1);

	waitpid(xbps_reconfigure, nullptr, 0);

	// Start netserver
	std::cout << "init: Starting netserver" << std::endl;
	auto netserver = fork();
	if(!netserver) {
		execl("/usr/bin/netserver", nullptr);
	}else assert(netserver != -1);

	// Start udev which loads the remaining drivers.
	std::cout << "init: Starting udevd" << std::endl;
	auto udev = fork();
	if(!udev) {
//		execl("/usr/sbin/udevd", "udevd", nullptr);
		execl("/usr/sbin/udevd", "udevd", "--debug", nullptr);
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

	// Start some drivers that are not integrated into udev rules yet.
#if defined (__x86_64__)
	auto input_ps2 = fork();
	if(!input_ps2) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run",
				"/usr/lib/managarm/server/input-atkbd.bin", nullptr);
	}else assert(input_ps2 != -1);
#endif

	auto input_hid = fork();
	if(!input_hid) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run",
				"/usr/lib/managarm/server/input-usbhid.bin", nullptr);
	}else assert(input_hid != -1);

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

	// Set the into blocking mode, so that we don't have to poll it.
	auto flags = fcntl(udev_monitor_get_fd(init_udev_mon), F_GETFL, 0);
	assert(flags >= 0);
	auto ret = fcntl(udev_monitor_get_fd(init_udev_mon), F_SETFL, flags & ~O_NONBLOCK);
	assert(!ret);

	std::cout << "init: udev initialization is done" << std::endl;

	// Determine what to launch.
	std::string launch = "kmscon";
	std::string command;

	std::string token;
	std::ifstream cmdline("/proc/cmdline");
	while(cmdline >> token) {
		if(!token.compare(0, 12, "init.launch=")) {
			launch = token.substr(12);
		}else if(!token.compare(0, 13, "init.command=")) {
			command = token.substr(13);
		}
	}

	// Wait until we have the devices required for weston/kmscon.
	bool need_drm = true;
	bool need_kbd = true;
	bool need_mouse = true;

	if(launch == "kmscon")
		need_mouse = false;
	if(launch == "headless") {
		need_drm = false;
		need_kbd = false;
		need_mouse = false;
	}

	std::cout << "init: Waiting for devices to show up" << std::endl;
	while(need_drm || need_kbd || need_mouse) {
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
			need_drm = false;
		}
		if(!strcmp(subsystem, "input")) {
			if(udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD")) {
				std::cout << "init: Found keyboard " << syspath << std::endl;
				need_kbd = false;
			}
			if(udev_device_get_property_value(dev, "ID_INPUT_MOUSE")) {
				std::cout << "init: Found mouse " << syspath << std::endl;
				need_mouse = false;
			}
		}

		udev_device_unref(dev);
	}

	// Finally, launch into kmscon/Weston.
	auto desktop = fork();
	if(!desktop) {
//		setenv("MLIBC_DEBUG_MALLOC", "1", 1);
		setenv("HOME", "/root", 1);
		setenv("XDG_RUNTIME_DIR", "/run", 1);
		setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
//		setenv("MESA_DEBUG", "1", 1);
		setenv("SHELL", "/bin/bash", 1);

		if(launch == "kmscon") {
			// TODO: kmscon should invoke a login program which sets these environment vars.
			// Force kmscon to not reset the environment
			execl("/usr/bin/kmscon", "kmscon", "--no-reset-env", nullptr);
			//execl("/usr/bin/kmscon", "kmscon", nullptr);
			//execl("/usr/bin/kmscon", "kmscon", "--debug", "--verbose", nullptr);
		}else if(launch == "weston") {
			// Hack: This should move to proper location
			if(mkdir("/tmp/.ICE-unix", 1777))
				throw std::runtime_error("mkdir() failed");
			if(mkdir("/tmp/.X11-unix", 1777))
				throw std::runtime_error("mkdir() failed");
			//execl("/usr/bin/weston", "weston", nullptr);
			execl("/usr/bin/weston", "weston", "--xwayland", nullptr);
			//execl("/usr/bin/weston", "weston", "--use-pixman", nullptr);
		}else if(launch == "headless") {
			auto fd = open("/dev/ttyS0", O_RDWR);
			if(fd < 0)
				throw std::runtime_error("Could not open /dev/ttyS0");
			if(dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
				throw std::runtime_error("dup2() failed");
			close(fd);
			execl(command.c_str(), command.c_str(), nullptr);
		}else if(launch == "sway") {
			setenv("WLR_RENDERER_ALLOW_SOFTWARE", "1", 1);
			execl("/usr/bin/seatd-launch", "seatd-launch", "--", "sway", nullptr);
		}else{
			std::cout << "init: init does not know how to launch " << launch << std::endl;
		}
		throw std::runtime_error("Could not execute desktop");
		//execl("/usr/bin/kmscube", "kmscube", nullptr);
	}else assert(desktop != -1);

	if(waitpid(desktop, nullptr, 0) < 0)
		throw std::runtime_error("waitpid() failed");
	std::cout << "init: Launched process terminated" << std::endl;

	while(true)
		sleep(60);
}
