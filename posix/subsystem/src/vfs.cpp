
#include "common.hpp"
#include "vfs.hpp"

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

StdSharedPtr<VfsOpenFile> VfsOpenFile::openAt(frigg::StringView path) {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}
void VfsOpenFile::write(const void *buffer, size_t length) {
	assert(!"Illegal operation for this file");
}
void VfsOpenFile::read(void *buffer, size_t max_length, size_t &actual_length) {
	assert(!"Illegal operation for this file");
}

void VfsOpenFile::setHelfd(HelHandle handle) {
	assert(!"Illegal operation for this file");
}
HelHandle VfsOpenFile::getHelfd() {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}

// --------------------------------------------------------
// MountSpace
// --------------------------------------------------------

MountSpace::MountSpace()
: allMounts(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

StdSharedPtr<VfsOpenFile> MountSpace::openAbsolute(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	assert(path.size() > 0);
	assert(path[0] == '/');
	
	// splits the path into a prefix that identifies the mount point
	// and a suffix that specifies remaining path relative to this mount point
	frigg::StringView prefix = path;
	frigg::StringView suffix;
	
	while(true) {
		auto mount = allMounts.get(prefix);
		if(mount)
			return (**mount)->openMounted(process, suffix, flags, mode);

		if(prefix == "/")
			return StdSharedPtr<VfsOpenFile>();

		size_t seperator = prefix.findLast('/');
		assert(seperator != size_t(-1));
		prefix = path.subString(0, seperator);
		suffix = path.subString(seperator + 1, path.size() - (seperator + 1));
	}
};


