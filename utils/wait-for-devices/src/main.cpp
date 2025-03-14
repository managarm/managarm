#include <functional>
#include <iostream>
#include <print>
#include <vector>

#include <CLI/CLI.hpp>
#include <fcntl.h>
#include <libudev.h>
#include <poll.h>
#include <string.h>

struct Predicate {
	const char *name{nullptr};
	std::function<bool(udev_device *)> detect;
};

std::vector<Predicate> pending;

bool debug = false;

int main(int argc, char **argv) {
	bool wantGraphics{false};
	bool wantMouse{false};
	bool wantKeyboard{false};

	CLI::App app{"wait-for-devices"};
	app.add_flag("--debug", debug);
	app.add_flag("--want-graphics", wantGraphics);
	app.add_flag("--want-keyboard", wantKeyboard);
	app.add_flag("--want-mouse", wantMouse);

	CLI11_PARSE(app, argc, argv);

	if (wantGraphics)
		pending.push_back(Predicate{
			.name = "graphics",
			.detect = [] (udev_device *udevDevice) {
				return udev_device_get_subsystem(udevDevice) == std::string_view{"drm"};
			}
		});
	if (wantKeyboard)
		pending.push_back(Predicate{
			.name = "keyboard",
			.detect = [] (udev_device *udevDevice) {
				return udev_device_get_property_value(udevDevice, "ID_INPUT_KEYBOARD") != nullptr;
			}
		});
	if (wantMouse)
		pending.push_back(Predicate{
			.name = "mouse",
			.detect = [] (udev_device *udevDevice) {
				return udev_device_get_property_value(udevDevice, "ID_INPUT_MOUSE") != nullptr;
			}
		});

	if (pending.empty())
		return 0;

	auto udevInstance = udev_new();
	if (!udevInstance) {
		std::println(std::cerr, "udev_new() failed");
		exit(1);
	}

	// Create a udev monitor to watch for new devices.
	auto udevMonitor = udev_monitor_new_from_netlink(udevInstance, "udev");
	if(!udevMonitor) {
		std::println(std::cerr, "udev_monitor_new_from_netlink() failed");
		exit(1);
	}
	if(udev_monitor_enable_receiving(udevMonitor) < 0) {
		std::println(std::cerr, "udev_monitor_enable_receiving() failed");
		exit(1);
	}

	// Do enumeration *after* creating the monitor. This finds already existing devices.
	auto udevEnum = udev_enumerate_new(udevInstance);
	if (!udevEnum) {
		std::println(std::cerr, "udev_enumerate_new() failed");
		exit(1);
	}

	if (udev_enumerate_scan_devices(udevEnum) < 0) {
		std::println(std::cerr, "udev_enumerate_scan_devices() failed");
		exit(1);
	}

	auto deviceList = udev_enumerate_get_list_entry(udevEnum);
	if (!deviceList) {
		std::println(std::cerr, "udev_enumerate_get_list_entry() failed");
		exit(1);
	}

	udev_list_entry *entry;
	udev_list_entry_foreach(entry, deviceList) {
		auto *syspath = udev_list_entry_get_name(entry);
		auto udevDevice = udev_device_new_from_syspath(udevInstance, syspath);
		if (!udevDevice) {
			std::println(std::cerr, "udev_device_new_from_syspath({}) failed", syspath);
			exit(1);
		}
		if (debug)
			std::println(std::cerr, "Enumeration probes {}", syspath);

		std::erase_if(pending, [&] (auto &predicate) {
			if (predicate.detect(udevDevice)) {
				std::println("Enumeration found {} at {}", predicate.name, syspath);
				return true;
			}
			return false;
		});

		udev_device_unref(udevDevice);
	}

	if (pending.empty())
		return 0;

	// Finally, wait until missing devices show up.
	std::println("Waiting for missing devices to show up");
	for (auto &predicate : pending)
		std::println("    Missing: {}", predicate.name);

	while(!pending.empty()) {
		pollfd pfd{.fd = udev_monitor_get_fd(udevMonitor), .events = POLLIN};
		if (int result = poll(&pfd, 1, -1); result < 0) {
			auto *error = strerror(errno);
			std::println(std::cerr, "poll() failed: {}", error);
			exit(1);
		}
		if (!(pfd.revents & POLLIN)) {
			std::println(std::cerr, "udev monitor is not readable");
			exit(1);
		}

		auto udevDevice = udev_monitor_receive_device(udevMonitor);
		if(!udevDevice) {
			std::println(std::cerr, "udev_monitor_receive_device() failed");
			exit(1);
		}
		auto syspath = udev_device_get_syspath(udevDevice);
		if (debug)
			std::println(std::cerr, "Monitor probes {}", syspath);

		std::erase_if(pending, [&] (auto &predicate) {
			if (predicate.detect(udevDevice)) {
				std::println("Monitor found {} at {}", predicate.name, syspath);
				return true;
			}
			return false;
		});

		udev_device_unref(udevDevice);
	}

	return 0;
}
