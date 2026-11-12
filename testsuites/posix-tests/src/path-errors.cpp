#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <frg/scope_exit.hpp>

#include "testsuite.hpp"

namespace {

std::string make_scratch() {
#if defined(__managarm__)
	std::string tmpl = "/posix-tests-patherr-XXXXXX";
#else
	std::string tmpl = "/tmp/posix-tests-patherr-XXXXXX";
#endif
	std::string path(tmpl);
	if(!mkdtemp(path.data()))
		assert(!"mkdtemp() failed");
	return path;
}

void create_file(const std::string &path) {
	int fd = creat(path.c_str(), 0644);
	assert(fd >= 0);
	close(fd);
}

} // namespace

// The expected errnos match Linux.

DEFINE_TEST(rmdir_rename_mount_point, ([] {
	errno = 0;
	assert(rmdir("/") == -1);
	assert(errno == EBUSY);

	errno = 0;
	assert(rename("/", "/posix-tests-patherr-renamed") == -1);
	assert(errno == EBUSY);

	// Linux checks the permissions of the parent before the mount point.
	if(getuid())
		return;

	errno = 0;
	assert(rmdir("/proc") == -1);
	assert(errno == EBUSY);

	errno = 0;
	assert(rename("/proc", "/posix-tests-patherr-renamed") == -1);
	assert(errno == EBUSY);
}))

DEFINE_TEST(create_onto_trailing_dot, ([] {
	auto dir = make_scratch();
	auto file = dir + "/file";
	auto dot = dir + "/.";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);

	errno = 0;
	assert(mkfifo(dot.c_str(), 0644) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(mknod(dot.c_str(), S_IFIFO | 0644, 0) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(symlink("target", dot.c_str()) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(link(file.c_str(), dot.c_str()) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(rename(file.c_str(), dot.c_str()) == -1);
	assert(errno == EBUSY);

	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(sock >= 0);
	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	assert(dot.size() < sizeof(addr.sun_path));
	strcpy(addr.sun_path, dot.c_str());
	errno = 0;
	assert(bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == -1);
	assert(errno == EADDRINUSE);
	close(sock);
}))

DEFINE_TEST(dirfd_not_directory, ([] {
	auto dir = make_scratch();
	auto file = dir + "/file";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);

	int epfd = epoll_create1(0);
	assert(epfd >= 0);
	int efd = eventfd(0, 0);
	assert(efd >= 0);
	int filefd = open(file.c_str(), O_RDONLY);
	assert(filefd >= 0);
	frg::scope_exit closeFds{[&] {
		close(filefd);
		close(efd);
		close(epfd);
	}};

	// Anonymous files have neither a mount nor a link.
	errno = 0;
	assert(openat(epfd, "x", O_RDWR | O_CREAT, 0644) == -1);
	assert(errno == ENOTDIR);

	errno = 0;
	assert(symlinkat("target", epfd, "x") == -1);
	assert(errno == ENOTDIR);

	errno = 0;
	assert(renameat(AT_FDCWD, file.c_str(), epfd, "x") == -1);
	assert(errno == ENOTDIR);

	errno = 0;
	assert(unlinkat(epfd, "", 0) == -1);
	assert(errno == ENOENT);

	errno = 0;
	assert(renameat(efd, "", AT_FDCWD, file.c_str()) == -1);
	assert(errno == ENOENT);

	errno = 0;
	assert(mkdirat(filefd, "x", 0755) == -1);
	assert(errno == ENOTDIR);

	// Absolute paths ignore the dirfd.
	int rootfd = openat(epfd, "/", O_RDONLY);
	assert(rootfd >= 0);
	close(rootfd);
}))
