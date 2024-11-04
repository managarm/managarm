
#include "file.hpp"

namespace inotify {

smarter::shared_ptr<File, FileHandle> createFile();
int addWatch(File *file, std::shared_ptr<FsNode> node, uint32_t mask);

} // namespace inotify
