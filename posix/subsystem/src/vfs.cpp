
#include "common.hpp"
#include "vfs.hpp"

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

void VfsOpenFile::openAt(frigg::StringView path,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}
	
void VfsOpenFile::fstat(frigg::CallbackPtr<void(FileStats)> callback) {
	assert(!"Illegal operation for this file");
}

void VfsOpenFile::write(const void *buffer, size_t length, frigg::CallbackPtr<void()> callback) {
	assert(!"Illegal operation for this file");
}
void VfsOpenFile::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(VfsError, size_t)> callback) {
	assert(!"Illegal operation for this file");
}

void VfsOpenFile::seek(int64_t rel_offset, frigg::CallbackPtr<void()> callback) {
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

void MountSpace::openAbsolute(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	assert(path.size() > 0);
	assert(path[0] == '/');
	assert(path[path.size() - 1] != '/');

	// splits the path into a prefix that identifies the mount point
	// and a suffix that specifies remaining path relative to this mount point
	frigg::StringView prefix = path;
	frigg::StringView suffix;
	
	while(true) {
		auto mount = allMounts.get(prefix);
		if(mount) {
			(*mount)->openMounted(process, suffix, flags, mode, callback);
			return;
		}
		
		// we failed to find a root mount point
		if(prefix == "") {
			callback(StdSharedPtr<VfsOpenFile>());
			return;
		}

		size_t seperator = prefix.findLast('/');
		assert(seperator != size_t(-1));
		prefix = path.subString(0, seperator);
		suffix = path.subString(seperator + 1, path.size() - (seperator + 1));
	}
};


