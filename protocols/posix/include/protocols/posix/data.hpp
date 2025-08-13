#pragma once

#include <hel.h>

namespace posix {

struct ManagarmRequestCancellationData {
	uint64_t cancellationId;
	HelHandle lane;
	int fd;
};

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	void *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
	// Shared memory page to hold the event to trigger
	// cancellation of current outstanding request.
	ManagarmRequestCancellationData *cancelRequestEvent;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
