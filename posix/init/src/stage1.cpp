
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
#include <unordered_set>
#include <ranges>
#include <filesystem>
#include <optional>
#include <fstream>

bool logDiscovery = false;

std::string findRootDevice() {
	std::cout << "init: Looking for the root partition" << std::endl;

	int nlFd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if(nlFd < 0) {
		std::cout << "init: socket(AF_NETLINK) failed! errno: " << strerror(errno) << std::endl;
		return "";
	}

	struct sockaddr_nl sa;
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = 1;

	int ret = bind(nlFd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
	if(ret < 0) {
		std::cout << "init: bind(nlFd) failed! errno: " << strerror(errno) << std::endl;
		return "";
	}

	std::unordered_set<std::string> checkedDevices;

	auto checkDevice = [&] (std::string device) -> std::optional<std::string> {
		if (checkedDevices.contains(device))
			return std::nullopt;

		checkedDevices.insert(device);

		if(logDiscovery)
			std::cout << "init: Considering device " << device << std::endl;

		auto rootAttr = device + "/managarm-root";

		// Check if the managarm-root attribute exists
		if(access(rootAttr.data(), R_OK)) {
			assert(errno == ENOENT);
			if(logDiscovery)
				std::cout << "init:  Not the root filesystem" << std::endl;
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
	};

	// Check existing devices in /sys/class/block
	// that appeared before we started listening
	for(auto dev : std::filesystem::directory_iterator{"/sys/class/block"}) {
		auto linkTarget = "/sys/class/block/" / std::filesystem::read_symlink(dev);

		if(logDiscovery)
			std::cout << "init: Candidate device " << linkTarget
				<< " found via /sys/class/block" << std::endl;
		if(auto device = checkDevice(linkTarget.generic_string()))
			return device.value();
	}

	while(true) {
		std::string buf;
		buf.resize(16384);

		ret = read(nlFd, buf.data(), buf.size());
		if(ret < 0) {
			std::cout << "init: read(nlFd) failed! errno: " << strerror(errno) << std::endl;
			return "";
		} else {
			// Parse the uevent message
			std::string_view action, subsystem;
			std::string devpath;

			const char *cur = buf.data();
			while(cur < buf.data() + ret) {
				std::string_view line{cur};
				auto split = line | std::views::split('=');
				auto it = split.begin();

				// TODO(qookie): C++23 would let us do std::string_view{*it}
				std::string_view name{(*it).begin(), (*it).end()};
				it++;
				std::string_view value{(*it).begin(), (*it).end()};

				if(name == "ACTION")
					action = value;
				else if(name == "SUBSYSTEM")
					subsystem = value;
				else if(name == "DEVPATH")
					devpath = "/sys" + std::string{value};

				cur += line.size() + 1;
			}

			if(action == "add" && subsystem == "block") {
				if(logDiscovery)
					std::cout << "init: Candidate device " << devpath
						<< " found via uevent" << std::endl;
				if(auto device = checkDevice(devpath))
					return device.value();
			}
		}
	}
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

	std::string rootPath = "";
	// TODO(qookie): Query /proc/cmdline to see if the user
	// requested a different device.

	if (!rootPath.size())
		rootPath = findRootDevice();
	if (!rootPath.size())
		throw std::runtime_error("Can't determine root device");

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	auto uhci = fork();
	if(!uhci) {
		execl("/bin/runsvr", "/bin/runsvr", "runsvr", "/sbin/uhci", nullptr);
	}else assert(uhci != -1);
#endif

	std::cout << "init: Mounting " << rootPath << std::endl;
	if(mount(rootPath.data(), "/realfs", "ext2", 0, ""))
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

