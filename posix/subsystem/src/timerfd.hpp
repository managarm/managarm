
#include <time.h>

#include "file.hpp"

namespace timerfd {

smarter::shared_ptr<File, FileHandle> createFile(int clock, bool non_block);
// Returns the previous setting in oldInitial and oldInterval.
void setTime(File *file, int flags, struct timespec initial, struct timespec interval,
		timespec &oldInitial, timespec &oldInterval);
void getTime(File *file, timespec &initial, timespec &interval);

} // namespace timerfd

