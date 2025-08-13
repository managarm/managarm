#pragma once

#include <hel.h>

namespace posix {

struct ThreadPage {
	unsigned int globalSignalFlag;
	uint64_t cancellationId;
	HelHandle lane;
	int fd;
};

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	ThreadPage *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
