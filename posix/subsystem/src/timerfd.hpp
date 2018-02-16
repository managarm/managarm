
#include <time.h>

#include "file.hpp"

namespace timerfd {

std::shared_ptr<ProxyFile> createFile();
void setTime(File *file, struct timespec initial, struct timespec interval);

} // namespace timerfd

