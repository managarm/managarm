
#include <string.h>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "vfs.hpp"

/*struct PathIterator {
	PathIterator(frigg::StringView path)
	: tail(path) { }

	operator bool() {
		return tail.size() > 0;
	}

	frigg::StringView operator* () {
		size_t slash = tail.findFirst('/');
		if(slash == size_t(-1))
			return tail;
		return tail.subString(0, slash);
	}

	void operator++ () {
		size_t slash = tail.findFirst('/');
		if(slash == size_t(-1)) {
			tail = frigg::StringView();
		}else{
			tail = tail.subString(slash + 1, tail.size() - (slash + 1));
		}
	}

private:
	frigg::StringView tail;
};

frigg::String<Allocator> normalizePath(frigg::StringView path) {
	PathIterator iterator(path);
	
	frigg::String<Allocator> result(*allocator);
	while(iterator) {
		if(*iterator != "" && *iterator != ".") {
			result += "/";
			result += *iterator;
		}
		++iterator;
	}
	if(result.size() == 0)
		return frigg::String<Allocator>(*allocator, "/");

	return frigg::move(result);
}

frigg::String<Allocator> concatenatePath(frigg::StringView prefix,
		frigg::StringView path) {
	PathIterator iterator(path);
	
	// the path is absolute: return it literally
	assert(iterator);
	if(*iterator == "")
		return frigg::String<Allocator>(*allocator, path);

	// the path is relative: we need to concatenate
	frigg::String<Allocator> result(*allocator, prefix);
	while(iterator) {
		result += "/";
		result += *iterator;
		++iterator;
	}
	
	return frigg::move(result);
}*/

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

COFIBER_ROUTINE(cofiber::future<void>, VfsOpenFile::readExactly(void *buffer,
		size_t length), ([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome((char *)buffer + offset,
				length - offset);
		assert(std::get<0>(result) == VfsError::success);
		assert(std::get<1>(result) > 0);
		offset += std::get<1>(result);
	}

	COFIBER_RETURN();
}))

cofiber::future<std::tuple<VfsError, size_t>> VfsOpenFile::readSome(void *buffer,
		size_t max_length) {
	throw std::runtime_error("vfs: Operation not supported");
}

cofiber::future<uint64_t> VfsOpenFile::seek(int64_t offset, VfsSeek whence) {
	throw std::runtime_error("vfs: Operation not supported");
}

cofiber::future<helix::UniqueDescriptor> VfsOpenFile::accessMemory() {
	throw std::runtime_error("vfs: Operation not supported");
}

/*void VfsOpenFile::openAt(frigg::String<Allocator> path,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}

void VfsOpenFile::connect(frigg::CallbackPtr<void()> callback) {
	assert(!"Illegal operation for this file");
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

void VfsOpenFile::seek(int64_t rel_offset, VfsSeek whence,
		frigg::CallbackPtr<void(uint64_t offset)> callback) {
	assert(!"Illegal operation for this file");
}

void VfsOpenFile::mmap(frigg::CallbackPtr<void(HelHandle)> callback) {
	assert(!"Illegal operation for this file");
}

frigg::Optional<frigg::String<Allocator>> VfsOpenFile::ttyName() {
	return frigg::Optional<frigg::String<Allocator>>();
}*/

/*
// --------------------------------------------------------
// MountSpace
// --------------------------------------------------------

MountSpace::MountSpace()
: allMounts(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

void MountSpace::openAbsolute(StdUnsafePtr<Process> process,
		frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	if(path == "/") {
		auto root = allMounts.get("");
		if(root) {
			(*root)->openMounted(process, frigg::String<Allocator>(*allocator, ""),
					flags, mode, callback);
		}else{
			callback(StdSharedPtr<VfsOpenFile>());
		}
		return;
	}

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
			(*mount)->openMounted(process, frigg::String<Allocator>(*allocator, suffix),
					flags, mode, callback);
			return;
		}
		
		// we failed to find a root mount point
		if(prefix == "") {
			callback(StdSharedPtr<VfsOpenFile>());
			return;
		}

		size_t seperator = prefix.findLast('/');
		assert(seperator != size_t(-1));
		prefix = frigg::StringView(path).subString(0, seperator);
		suffix = frigg::StringView(path).subString(seperator + 1, path.size() - (seperator + 1));
	}
};*/


