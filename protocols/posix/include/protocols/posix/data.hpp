#pragma once

#include <hel.h>

namespace posix {

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	void *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
	// Shared memory page to hold the event to trigger
	// cancellation of current outstanding request.
	HelHandle *cancelRequestEvent;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
