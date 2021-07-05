#pragma once

#include <hel.h>

namespace posix {

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	void *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
