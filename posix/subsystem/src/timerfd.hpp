
#include <time.h>

#include "file.hpp"

namespace timerfd {

smarter::shared_ptr<File, FileHandle> createFile(bool non_block);
void setTime(File *file, struct timespec initial, struct timespec interval);

} // namespace timerfd

