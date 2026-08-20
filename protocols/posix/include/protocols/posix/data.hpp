#pragma once

#include <hel.h>

namespace posix {

struct ThreadPage {
	// Signal delivery protocol between mlibc's SignalGuard and the posix server.
	//   0: No SignalGuard is active. Signals are raised immediately.
	//   1: A SignalGuard is active. Signals must not be raised.
	//   2: A SignalGuard is active. A signal has been accepted but not raised yet.
	//      The SignalGuard's exit path must invoke superSigRaise to enter the signal handler.
	//      While the flag is 2, in-flight cancellable operations must be cancelled so that the
	//      SignalGuard region can end (returning EINTR as appropriate).
	//
	// mlibc stores 1 on SignalGuard entry and atomically exchanges to zero on SignalGuard exit.
	// posix-subsystem performs the 1 -> 2 transition while the thread is interrupted
	// and alerts the queue to cancel waits that have already passed their SignalGuard checks.
	unsigned int globalSignalFlag;
	HelHandle queueHandle;
};

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	ThreadPage *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
};

} // namespace posix
