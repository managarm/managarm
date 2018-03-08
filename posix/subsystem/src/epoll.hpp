
#include <sys/epoll.h>

#include "file.hpp"

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile();

void addItem(File *epfile, File *file, int flags, uint64_t cookie);
void modifyItem(File *epfile, File *file, int flags, uint64_t cookie);
void deleteItem(File *epfile, File *file, int flags);

async::cancelable_result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events);

} // namespace epoll

