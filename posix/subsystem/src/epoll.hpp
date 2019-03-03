
#include <sys/epoll.h>

#include <async/cancellation.hpp>
#include "file.hpp"

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile();

void addItem(File *epfile, Process *process, smarter::shared_ptr<File> file,
		int flags, uint64_t cookie);
void modifyItem(File *epfile, File *file, int flags, uint64_t cookie);
void deleteItem(File *epfile, File *file, int flags);

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events, async::cancellation_token cancellation = {});

} // namespace epoll

