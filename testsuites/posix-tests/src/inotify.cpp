#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "testsuite.hpp"

DEFINE_TEST(inotify_unlink_child, ([] {
	int e;

	char dirPath[32];
	strcpy(dirPath, "/tmp/posix-tests.XXXXXX");
	if(!mkdtemp(dirPath))
		assert(!"mkdtemp() failed");

	char filePath[64];
	sprintf(filePath, "%s/foobar", dirPath);
	int ffd = creat(filePath, 0644);
	assert(ffd > 0);
	close(ffd);

	int ifd = inotify_init();
	assert(ifd > 0);
	int wd = inotify_add_watch(ifd, dirPath, IN_DELETE);
	assert(wd >= 0);

	// Trigger inotify.
	e = unlink(filePath);
	assert(!e);

	char buffer[(sizeof(inotify_event) + NAME_MAX + 1) * 2];
	auto chunk = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	assert(chunk > 0);

	inotify_event evtHeader;
	memcpy(&evtHeader, buffer, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert(evtHeader.mask & IN_DELETE);

	std::string evtName{buffer + sizeof(inotify_event), strnlen(buffer + sizeof(inotify_event), evtHeader.len)};
	assert(evtName == "foobar");

	close(ifd);
	//e = rmdir(dirPath);
	//assert(!e);

	ifd = inotify_init1(IN_NONBLOCK);
	assert(ifd > 0);
	char testfile[27] = "/tmp/posix-test-fileXXXXXX";
	int fd = mkstemp(testfile);
	assert(fd >= 0);

	int ifd2 = inotify_init1(IN_NONBLOCK);
	assert(ifd2 > 0);

	wd = inotify_add_watch(ifd, testfile, IN_MODIFY | IN_ACCESS | IN_DELETE_SELF | IN_CLOSE_WRITE);
	assert(wd >= 0);

	write(fd, &fd, sizeof(fd));
	lseek(fd, 0, SEEK_SET);
	int discard;
	read(fd, &discard, sizeof(discard));
	write(fd, &fd, sizeof(fd));

	memset(buffer, 0, sizeof(buffer));
	chunk = read(ifd, buffer, sizeof(buffer));
	assert(chunk > 0 && size_t(chunk) >= sizeof(inotify_event));

	memcpy(&evtHeader, buffer, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert((evtHeader.mask & IN_MODIFY) == IN_MODIFY);
	assert(size_t(chunk) > sizeof(inotify_event) + evtHeader.len);

	memcpy(&evtHeader, buffer + sizeof(inotify_event) + evtHeader.len, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert((evtHeader.mask & IN_ACCESS) == IN_ACCESS);

	memset(buffer, 0, sizeof(buffer));
	chunk = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	assert(chunk == -1);

	int wd2 = inotify_add_watch(ifd2, testfile, IN_MODIFY | IN_ACCESS | IN_DELETE_SELF | IN_CLOSE_WRITE);
	assert(wd2 >= 0);
	inotify_rm_watch(ifd2, wd2);

	memset(buffer, 0, sizeof(buffer));
	chunk = read(ifd2, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	assert(chunk > 0);

	memcpy(&evtHeader, buffer, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert((evtHeader.mask & IN_IGNORED) == IN_IGNORED);

	close(fd);

	memset(buffer, 0, sizeof(buffer));
	chunk = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	assert(chunk > 0);

	memcpy(&evtHeader, buffer, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert((evtHeader.mask & IN_CLOSE_WRITE) == IN_CLOSE_WRITE);

	e = unlink(testfile);
	assert(!e);

	memset(buffer, 0, sizeof(buffer));
	chunk = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	assert(chunk > 0);

	memcpy(&evtHeader, buffer, sizeof(inotify_event));
	assert(evtHeader.wd == wd);
	assert((evtHeader.mask & IN_DELETE_SELF) == IN_DELETE_SELF);
}))

DEFINE_TEST(inotify_fionread, ([] {
	int ret = 0;
	char dirPath[] = "/tmp/posix-tests-inotify.XXXXXX";
	auto mkdtempResult = mkdtemp(dirPath);
	assert(mkdtempResult);

	int ifd = inotify_init();
	assert(ifd >= 0);
	int wd = inotify_add_watch(ifd, dirPath, IN_CREATE);
	assert(wd >= 0);

	const char *names[] = {"a", "a-longer-name"};
	for(const char *name : names) {
		std::string path = std::string{dirPath} + "/" + name;
		int fd = creat(path.c_str(), 0644);
		assert(fd >= 0);
		ret = close(fd);
		assert(!ret);
	}

	pollfd pfd{ifd, POLLIN, 0};
	ret = poll(&pfd, 1, 1000);
	assert(ret == 1);
	assert(pfd.revents & POLLIN);

	int pending = -1;
	ret = ioctl(ifd, FIONREAD, &pending);
	assert(!ret);
	assert(pending >= 2 * static_cast<int>(sizeof(inotify_event)));

	std::vector<char> buffer(pending);
	ssize_t chunk = read(ifd, buffer.data(), buffer.size());
	assert(chunk == pending);

	int remaining = -1;
	ret = ioctl(ifd, FIONREAD, &remaining);
	assert(!ret);
	assert(!remaining);

	bool sawShortName = false;
	bool sawLongName = false;
	size_t offset = 0;
	while(offset < buffer.size()) {
		assert(buffer.size() - offset >= sizeof(inotify_event));
		inotify_event event;
		memcpy(&event, buffer.data() + offset, sizeof(event));
		assert(event.wd == wd);
		assert(event.mask & IN_CREATE);

		size_t eventSize = sizeof(inotify_event) + event.len;
		assert(eventSize <= buffer.size() - offset);
		const char *eventName = buffer.data() + offset + sizeof(inotify_event);
		std::string name{eventName, strnlen(eventName, event.len)};
		sawShortName |= name == names[0];
		sawLongName |= name == names[1];
		offset += eventSize;
	}
	assert(offset == buffer.size());
	assert(sawShortName && sawLongName);

	ret = close(ifd);
	assert(!ret);
	for(const char *name : names) {
		std::string path = std::string{dirPath} + "/" + name;
		ret = unlink(path.c_str());
		assert(!ret);
	}
	ret = rmdir(dirPath);
	assert(!ret);
}))
