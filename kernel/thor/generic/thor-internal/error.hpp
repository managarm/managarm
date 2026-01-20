#pragma once

namespace thor {

enum class Error {
	success,
	illegalArgs,
	illegalObject,
	illegalState,
	outOfBounds,
	cancelled,
	futexRace,
	bufferTooSmall,
	threadExited,
	transmissionMismatch,
	laneShutdown,
	endOfLane,
	dismissed,
	fault,
	remoteFault,
	noMemory,
	noHardwareSupport,
	alreadyExists,
	badPermissions,
	// Internal error: the remote has violated the IPC protocol.
	hardwareBroken,
	protocolViolation,
	spuriousOperation,
};

} // namespace thor
