#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <frg/scope_exit.hpp>

#include "testsuite.hpp"

namespace {

// Per-test scratch directory: the ext2 root on Managarm (to exercise libblockfs,
// which /tmp as tmpfs would bypass), /tmp on hosts where the root is not writable.
std::string make_scratch() {
#if defined(__managarm__)
	std::string tmpl = "/posix-tests-rename-XXXXXX";
#else
	std::string tmpl = "/tmp/posix-tests-rename-XXXXXX";
#endif
	std::string path(tmpl);
	if(!mkdtemp(path.data()))
		assert(!"mkdtemp() failed");
	return path;
}

void create_file(const std::string &path, const char *data = nullptr) {
	int fd = creat(path.c_str(), 0644);
	assert(fd >= 0);
	if(data) {
		auto len = strlen(data);
		auto written = write(fd, data, len);
		assert(static_cast<size_t>(written) == len);
	}
	close(fd);
}

bool path_exists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

} // namespace

// rename(missing, *) must fail with ENOENT.
DEFINE_TEST(rename_missing_source, ([] {
	auto dir = make_scratch();
	frg::scope_exit cleanup{[&] { rmdir(dir.c_str()); }};

	auto src = dir + "/missing";
	auto dst = dir + "/dst";

	errno = 0;
	int e = rename(src.c_str(), dst.c_str());
	assert(e == -1);
	assert(errno == ENOENT);
	assert(!path_exists(dst));
}))

// Basic rename of a regular file to a new name in the same directory.
DEFINE_TEST(rename_file_basic, ([] {
	auto dir = make_scratch();
	auto src = dir + "/a";
	auto dst = dir + "/b";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		unlink(dst.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src, "hello");

	int e = rename(src.c_str(), dst.c_str());
	assert(e == 0);
	assert(!path_exists(src));
	assert(path_exists(dst));

	struct stat st;
	assert(stat(dst.c_str(), &st) == 0);
	assert(st.st_size == 5);
}))

// Rename onto an existing regular file replaces the destination.
DEFINE_TEST(rename_file_over_file, ([] {
	auto dir = make_scratch();
	auto src = dir + "/a";
	auto dst = dir + "/b";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		unlink(dst.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src, "from-src");
	create_file(dst, "from-dst");

	int e = rename(src.c_str(), dst.c_str());
	assert(e == 0);
	assert(!path_exists(src));

	int fd = open(dst.c_str(), O_RDONLY);
	assert(fd >= 0);
	char buf[16] = {};
	auto n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	assert(n == 8);
	assert(std::string(buf) == "from-src");
}))

// Rename of an empty directory to a new name.
DEFINE_TEST(rename_dir_basic, ([] {
	auto dir = make_scratch();
	auto src = dir + "/srcdir";
	auto dst = dir + "/dstdir";
	frg::scope_exit cleanup{[&] {
		rmdir(src.c_str());
		rmdir(dst.c_str());
		rmdir(dir.c_str());
	}};

	int e = mkdir(src.c_str(), 0755);
	assert(e == 0);

	e = rename(src.c_str(), dst.c_str());
	assert(e == 0);
	assert(!path_exists(src));

	struct stat st;
	assert(stat(dst.c_str(), &st) == 0);
	assert(S_ISDIR(st.st_mode));
}))

// Rename onto an existing non-empty directory must fail with ENOTEMPTY.
DEFINE_TEST(rename_onto_nonempty_dir, ([] {
	auto dir = make_scratch();
	auto src = dir + "/srcdir";
	auto dst = dir + "/dstdir";
	auto dst_child = dst + "/child";
	frg::scope_exit cleanup{[&] {
		unlink(dst_child.c_str());
		rmdir(src.c_str());
		rmdir(dst.c_str());
		rmdir(dir.c_str());
	}};

	int e = mkdir(src.c_str(), 0755);
	assert(e == 0);
	e = mkdir(dst.c_str(), 0755);
	assert(e == 0);
	create_file(dst_child);

	errno = 0;
	e = rename(src.c_str(), dst.c_str());
	assert(e == -1);
	assert(errno == ENOTEMPTY || errno == EEXIST);

	// Both directories must still exist with their original contents.
	struct stat st;
	assert(stat(src.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
	assert(stat(dst.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
	assert(path_exists(dst_child));
}))

// Rename within the same directory removes the source name.
DEFINE_TEST(rename_in_same_directory, ([] {
	auto dir = make_scratch();
	auto src = dir + "/a";
	auto dst = dir + "/b";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		unlink(dst.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src);
	int e = rename(src.c_str(), dst.c_str());
	assert(e == 0);
	assert(!path_exists(src));
	assert(path_exists(dst));
}))

// Renaming a non-directory onto an existing directory must fail with EISDIR.
DEFINE_TEST(rename_file_onto_dir_fails, ([] {
	auto dir = make_scratch();
	auto src = dir + "/file";
	auto dst = dir + "/dir";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		rmdir(dst.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src);
	assert(mkdir(dst.c_str(), 0755) == 0);

	errno = 0;
	int e = rename(src.c_str(), dst.c_str());
	assert(e == -1);
	assert(errno == EISDIR);
	assert(path_exists(src));
	assert(path_exists(dst));
}))

// Renaming a directory onto an existing non-directory must fail with ENOTDIR.
DEFINE_TEST(rename_dir_onto_file_fails, ([] {
	auto dir = make_scratch();
	auto src = dir + "/dir";
	auto dst = dir + "/file";
	frg::scope_exit cleanup{[&] {
		rmdir(src.c_str());
		unlink(dst.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(src.c_str(), 0755) == 0);
	create_file(dst);

	errno = 0;
	int e = rename(src.c_str(), dst.c_str());
	assert(e == -1);
	assert(errno == ENOTDIR);
	assert(path_exists(src));
	assert(path_exists(dst));
}))

// Moving a directory to a new parent repoints its ".." and shifts the
// subdirectory link from the old to the new parent.
DEFINE_TEST(rename_dir_to_new_parent, ([] {
	auto dir = make_scratch();
	auto p1 = dir + "/p1";
	auto p2 = dir + "/p2";
	auto src = p1 + "/d";
	auto dst = p2 + "/d";
	frg::scope_exit cleanup{[&] {
		rmdir(src.c_str());
		rmdir(dst.c_str());
		rmdir(p1.c_str());
		rmdir(p2.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(p1.c_str(), 0755) == 0);
	assert(mkdir(p2.c_str(), 0755) == 0);
	assert(mkdir(src.c_str(), 0755) == 0);

	struct stat p1_before, p2_before;
	assert(stat(p1.c_str(), &p1_before) == 0);
	assert(stat(p2.c_str(), &p2_before) == 0);

	assert(rename(src.c_str(), dst.c_str()) == 0);
	assert(!path_exists(src));
	assert(path_exists(dst));

	// ".." of the moved directory now resolves to the new parent.
	struct stat dotdot, p2_stat;
	assert(stat((dst + "/..").c_str(), &dotdot) == 0);
	assert(stat(p2.c_str(), &p2_stat) == 0);
	assert(dotdot.st_ino == p2_stat.st_ino);

	// The old parent loses a link, the new parent gains one.
	struct stat p1_after, p2_after;
	assert(stat(p1.c_str(), &p1_after) == 0);
	assert(stat(p2.c_str(), &p2_after) == 0);
	assert(p1_after.st_nlink == p1_before.st_nlink - 1);
	assert(p2_after.st_nlink == p2_before.st_nlink + 1);
}))

// Replacing an empty directory in another parent repoints ".." but leaves the
// new parent's link count unchanged, since the replaced directory already
// contributed it.
DEFINE_TEST(rename_dir_onto_empty_dir_in_new_parent, ([] {
	auto dir = make_scratch();
	auto p1 = dir + "/p1";
	auto p2 = dir + "/p2";
	auto src = p1 + "/d";
	auto dst = p2 + "/d";
	frg::scope_exit cleanup{[&] {
		rmdir(src.c_str());
		rmdir(dst.c_str());
		rmdir(p1.c_str());
		rmdir(p2.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(p1.c_str(), 0755) == 0);
	assert(mkdir(p2.c_str(), 0755) == 0);
	assert(mkdir(src.c_str(), 0755) == 0);
	assert(mkdir(dst.c_str(), 0755) == 0);

	struct stat p1_before, p2_before;
	assert(stat(p1.c_str(), &p1_before) == 0);
	assert(stat(p2.c_str(), &p2_before) == 0);

	assert(rename(src.c_str(), dst.c_str()) == 0);
	assert(!path_exists(src));
	assert(path_exists(dst));

	struct stat dotdot, p2_stat;
	assert(stat((dst + "/..").c_str(), &dotdot) == 0);
	assert(stat(p2.c_str(), &p2_stat) == 0);
	assert(dotdot.st_ino == p2_stat.st_ino);

	struct stat p1_after, p2_after;
	assert(stat(p1.c_str(), &p1_after) == 0);
	assert(stat(p2.c_str(), &p2_after) == 0);
	assert(p1_after.st_nlink == p1_before.st_nlink - 1);
	assert(p2_after.st_nlink == p2_before.st_nlink);
}))

// Renaming a file onto itself is a no-op and must not destroy it.
DEFINE_TEST(rename_same_path_file, ([] {
	auto dir = make_scratch();
	auto src = dir + "/file";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src, "payload");

	assert(rename(src.c_str(), src.c_str()) == 0);
	assert(path_exists(src));

	char buf[8] = {};
	int fd = open(src.c_str(), O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, buf, sizeof(buf) - 1) == 7);
	close(fd);
	assert(!strcmp(buf, "payload"));
}))

// Renaming a (non-empty) directory onto itself is a no-op and must not destroy
// it or its contents.
DEFINE_TEST(rename_same_path_dir, ([] {
	auto dir = make_scratch();
	auto sub = dir + "/sub";
	auto child = sub + "/child";
	frg::scope_exit cleanup{[&] {
		unlink(child.c_str());
		rmdir(sub.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(sub.c_str(), 0755) == 0);
	create_file(child);

	assert(rename(sub.c_str(), sub.c_str()) == 0);
	assert(path_exists(sub));
	assert(path_exists(child));
}))

// Renaming one hard link onto another link of the same file is a no-op: both
// names must survive.
DEFINE_TEST(rename_hardlink_same_inode, ([] {
	auto dir = make_scratch();
	auto a = dir + "/a";
	auto b = dir + "/b";
	frg::scope_exit cleanup{[&] {
		unlink(a.c_str());
		unlink(b.c_str());
		rmdir(dir.c_str());
	}};

	create_file(a);
	assert(link(a.c_str(), b.c_str()) == 0);

	assert(rename(a.c_str(), b.c_str()) == 0);
	assert(path_exists(a));
	assert(path_exists(b));

	struct stat sa, sb;
	assert(stat(a.c_str(), &sa) == 0);
	assert(stat(b.c_str(), &sb) == 0);
	assert(sa.st_ino == sb.st_ino);
	assert(sa.st_nlink == 2);
}))

// Renaming a directory into itself or one of its own descendants must fail
// with EINVAL rather than detaching the subtree.
DEFINE_TEST(rename_dir_into_descendant, ([] {
	auto dir = make_scratch();
	auto a = dir + "/a";
	auto b = a + "/b";
	frg::scope_exit cleanup{[&] {
		rmdir(b.c_str());
		rmdir(a.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(a.c_str(), 0755) == 0);
	assert(mkdir(b.c_str(), 0755) == 0);

	// Moving "a" directly into itself.
	errno = 0;
	assert(rename(a.c_str(), (a + "/x").c_str()) == -1);
	assert(errno == EINVAL);

	// Moving "a" into its descendant "a/b".
	errno = 0;
	assert(rename(a.c_str(), (b + "/x").c_str()) == -1);
	assert(errno == EINVAL);

	// The tree is left untouched.
	assert(path_exists(a));
	assert(path_exists(b));
}))
