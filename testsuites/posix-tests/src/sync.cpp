#include <cassert>
#include <cerrno>
#include <cstdlib>

#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(synchronize_file, ([] {
	char path[] = "/tmp/posix-tests-sync-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);

	char data[] = "synchronized data";
	assert(write(fd, data, sizeof(data)) == sizeof(data));
	assert(fdatasync(fd) == 0);

	data[0] = 'S';
	assert(pwrite(fd, data, sizeof(data), 0) == sizeof(data));
	assert(fsync(fd) == 0);
	assert(syncfs(fd) == 0);

	// Fast symlinks store their contents inside the inode and have no file-data
	// pages to synchronize.
	char linkPath[] = "/tmp/posix-tests-sync-link-XXXXXX";
	int linkFd = mkstemp(linkPath);
	assert(linkFd >= 0);
	assert(close(linkFd) == 0);
	assert(unlink(linkPath) == 0);
	assert(symlink("short-target", linkPath) == 0);

	sync();

	assert(unlink(linkPath) == 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);
}))

DEFINE_TEST(synchronize_badfd, ([] {
	constexpr int bogusFd = 1947830128;

	errno = 0;
	assert(fsync(bogusFd) == -1);
	assert(errno == EBADF);

	errno = 0;
	assert(fdatasync(bogusFd) == -1);
	assert(errno == EBADF);

	errno = 0;
	assert(syncfs(bogusFd) == -1);
	assert(errno == EBADF);
}))
