
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <linux/netlink.h>
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <ranges>
#include <filesystem>
#include <optional>
#include <fstream>

bool logDiscovery = false;

using Uevent = std::unordered_map<std::string, std::string>;

// This class implements a udevd-like mechanism to discover devices via netlink uevents.
class UeventEngine {
public:
	void init() {
		int ret;

		nlFd_ = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
		if(nlFd_ < 0) {
			std::cout << "init: socket(AF_NETLINK) failed! errno: " << strerror(errno) << std::endl;
			return;
		}

		struct sockaddr_nl sa;
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = getpid();
		sa.nl_groups = 1;

		ret = bind(nlFd_, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
		if(ret < 0) {
			std::cout << "init: bind(nlFd) failed! errno: " << strerror(errno) << std::endl;
			return;
		}
	}

	// Trigger synthetic uevents that are handled by nextUevent().
	void trigger() {
		for(auto dev : std::filesystem::recursive_directory_iterator{"/sys/devices/"}) {
			if (!dev.is_directory())
				continue;
			auto ueventPath = "/sys/devices/" / dev.path() / "uevent";
			if (!std::filesystem::exists(ueventPath))
				continue;
			if (logDiscovery)
				std::cout << "Triggering " << ueventPath << std::endl;

			int fd = open(ueventPath.c_str(), O_WRONLY);
			if (fd < 0) {
				std::cout << "Failed open " << ueventPath << " to trigger uevent" << std::endl;
				continue;
			}
			std::string_view s = "add";
			if (write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()))
				std::cout << "Failed to write to uevent file " << ueventPath << std::endl;
			close(fd);
		}
	}

	std::optional<Uevent> nextUevent() {
		int ret;

		while(true) {
			std::string buf;
			buf.resize(16384);

			ret = read(nlFd_, buf.data(), buf.size());
			if(ret < 0) {
				std::cout << "init: read(nlFd) failed! errno: " << strerror(errno) << std::endl;
				return std::nullopt;
			}

			// Parse the uevent message
			Uevent uevent;

			const char *cur = buf.data();
			while(cur < buf.data() + ret) {
				std::string_view line{cur};
				auto split = line | std::views::split('=');
				auto it = split.begin();

				// TODO(qookie): C++23 would let us do std::string_view{*it}
				std::string_view name{(*it).begin(), (*it).end()};
				it++;
				std::string_view value{(*it).begin(), (*it).end()};

				uevent[std::string(name)] = value;

				cur += line.size() + 1;
			}

			const auto &action = uevent.at("ACTION");
			const auto &devpath = uevent.at("DEVPATH");
			if (action != "add")
				continue;
			if (knownDevices_.contains(devpath))
				continue;
			knownDevices_.insert(devpath);
			return uevent;
		}
	}

private:
	int nlFd_{-1};
	std::unordered_set<std::string> knownDevices_;
};

std::optional<std::string> checkRootDevice(std::string device) {
	if(logDiscovery)
		std::cout << "init: Considering device " << device << std::endl;

	auto rootAttr = device + "/managarm-root";

	// Check if the managarm-root attribute exists
	if(access(rootAttr.data(), R_OK)) {
		assert(errno == ENOENT);
		if(logDiscovery)
			std::cout << "init: Not the root filesystem" << std::endl;
		return std::nullopt;
	}

	// Figure out the device's major:minor
	std::ifstream ifs{device + "/dev"};
	std::string dev;
	std::getline(ifs, dev);

	auto split = dev | std::views::split(':');
	auto it = split.begin();

	// TODO(qookie): C++23 would let us do std::string_view{*it}
	std::string majorStr{(*it).begin(), (*it).end()};
	it++;
	std::string minorStr{(*it).begin(), (*it).end()};

	int major = std::stoi(majorStr), minor = std::stoi(minorStr);

	// Find the /dev node with the right major:minor numbers
	for(auto node : std::filesystem::directory_iterator{"/dev/"}) {
		struct stat st;
		stat(node.path().c_str(), &st);

		if(st.st_rdev == makedev(major, minor)) {
			return node.path();
		}
	}

	// This major:minor is not in /dev? Bail out...
	std::cout << "init: Device " << device << " (maj:min " << major << ":" << minor << ")"
		<< "is the root filesystem, but has no corresponding /dev node?" << std::endl;
	return "";
}

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
#if defined (__x86_64__)
	auto ehci = fork();
	if(!ehci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/ehci", nullptr);
	}else assert(ehci != -1);
#endif

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

	std::optional<std::string> rootPath;
	// TODO(qookie): Query /proc/cmdline to see if the user
	// requested a different device.

	UeventEngine ueventEngine;
	std::cout << "init: Looking for the root partition" << std::endl;

	ueventEngine.init();
	ueventEngine.trigger();

	while (!rootPath) {
		auto uevent = ueventEngine.nextUevent();
		if (!uevent) {
			std::cout << "Failed to receive uevent" << std::endl;
			abort();
		}

		if(logDiscovery) {
			std::cout << "init: Received uevent";
			for (const auto &kv : *uevent)
				std::cout << "\n    " << kv.first << "=" << kv.second;
			std::cout << std::endl;
		}

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent->at("DEVPATH");
		auto subsystemIt = uevent->find("SUBSYSTEM");

		if (subsystemIt != uevent->end() && subsystemIt->second == "block")
			rootPath = checkRootDevice("/sys" + devpath);
	}
	if (!rootPath->size())
		throw std::runtime_error("Can't determine root device");

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	auto uhci = fork();
	if(!uhci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/uhci", nullptr);
	}else assert(uhci != -1);
#endif

	std::cout << "init: Mounting " << *rootPath << std::endl;
	if(mount(rootPath->data(), "/realfs", "ext2", 0, ""))
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

	// /run/utmp must exist for login to be satisfied.
	int utmp = open("/run/utmp", O_CREAT, O_RDWR);
	if(utmp == -1)
		throw std::runtime_error("Opening /run/utmp failed");
	close(utmp);

	// Symlink /var/run to /run, just like LFS does
	int varrun = symlink("/run", "/var/run");
	if(varrun == -1)
		throw std::runtime_error("Symlinking /var/run failed");

	execl("/usr/bin/init-stage2", "/usr/bin/init-stage2", nullptr);
	std::cout << "init: Failed to execve() second stage" << std::endl;
	abort();
}

