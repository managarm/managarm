#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(inotify_unlink_child, ([] {
	            int e;

	            char dirPath[64];
	            strcpy(dirPath, "/tmp/posix-tests.XXXXXX");
	            if (!mkdtemp(dirPath))
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

	            char buffer[sizeof(inotify_event) + NAME_MAX + 1];
	            auto chunk = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX + 1);
	            assert(chunk > 0);

	            inotify_event evtHeader;
	            memcpy(&evtHeader, buffer, sizeof(inotify_event));
	            assert(evtHeader.wd == wd);
	            assert(evtHeader.mask & IN_DELETE);

	            std::string evtName{buffer + sizeof(inotify_event), evtHeader.len};
	            assert(evtName == "foobar");

	            close(ifd);
	            // e = rmdir(dirPath);
	            // assert(!e);
            }))
