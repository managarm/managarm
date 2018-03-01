
#include <time.h>

#include "file.hpp"

namespace timerfd {

std::shared_ptr<File> createFile(bool non_block);
void setTime(File *file, struct timespec initial, struct timespec interval);

} // namespace timerfd

