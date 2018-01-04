
#include <sys/epoll.h>

#include "file.hpp"

std::shared_ptr<ProxyFile> createEpollFile();

void epollCtl(File *epfile, File *file, int flags, uint64_t cookie);

async::result<struct epoll_event> epollWait(File *epfile);

