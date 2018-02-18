
#include <sys/epoll.h>

#include "file.hpp"

namespace epoll {

std::shared_ptr<ProxyFile> createFile();

void addItem(File *epfile, File *file, int flags, uint64_t cookie);
void modifyItem(File *epfile, File *file, int flags, uint64_t cookie);
void deleteItem(File *epfile, File *file, int flags);

async::result<struct epoll_event> wait(File *epfile);

} // namespace epoll

