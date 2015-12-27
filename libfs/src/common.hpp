
#ifndef LIBFS_COMMON_H
#define LIBFS_COMMON_H

namespace libfs {

enum FileType {
	kTypeNone,
	kTypeRegular,
	kTypeDirectory,
	kTypeSymlink
};

} // namespace libfs

#endif // LIBFS_COMMON_H

