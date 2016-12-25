
#ifndef LIBFS_COMMON_H
#define LIBFS_COMMON_H

namespace blockfs {

enum FileType {
	kTypeNone,
	kTypeRegular,
	kTypeDirectory,
	kTypeSymlink
};

} // namespace blockfs

#endif // LIBFS_COMMON_H

