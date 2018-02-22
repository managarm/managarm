
#include <time.h>

#include "file.hpp"

namespace timerfd {

std::shared_ptr<File> createFile();
void setTime(File *file, struct timespec initial, struct timespec interval);

} // namespace timerfd

