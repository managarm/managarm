#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(mkdir_trailing_dot, ([] {
	rmdir("a");

	int ret = open("a/.", O_RDWR | O_CREAT, 0666);
	assert(ret == -1);
	assert(errno == ENOENT);

	ret = mkdir("a/.", 0777);
	assert(ret == -1);
	assert(errno == ENOENT);

	mkdir("a", 0777);

	ret = open("a/.", O_RDWR | O_CREAT, 0666);
	assert(ret == -1);
	assert(errno == EISDIR);
	ret = open("a/.", O_RDONLY | O_CREAT | O_EXCL, 0666);
	assert(ret == -1);
	assert(errno == EEXIST);
	ret = open("a/.", O_RDONLY, 0666);
	assert(ret > 0);

	ret = open("/..", O_RDONLY, 0666);
	assert(ret > 0);

	char overlong_path[PATH_MAX + 3];
	memset(overlong_path, '1', PATH_MAX + 1);
	overlong_path[PATH_MAX + 2] = '\0';
	ret = open(overlong_path, O_RDONLY | O_CREAT, 0666);
	assert(ret == -1);
	assert(errno == ENAMETOOLONG);

	char overlong_component[] = "a/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	ret = open(overlong_component, O_RDONLY | O_CREAT, 0666);
	assert(ret == -1);
	assert(errno == ENAMETOOLONG);

	struct rlimit limit{32, 32};
	setrlimit(RLIMIT_NOFILE, &limit);

	bool gotEMFILE = false;
	for(size_t i = 0 ; i < 32; i++) {
		ret = open("/dev/null", O_RDWR);
		if (ret == -1 && errno == EMFILE) {
			gotEMFILE = true;
			break;
		}
	}
	assert(gotEMFILE);

	rmdir("a");
}))
