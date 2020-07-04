#pragma once

namespace thor {

enum class Error {
	success,
	illegalArgs,
	illegalObject,
	illegalState,
	outOfBounds,
	cancelled,
	bufferTooSmall,
	threadExited,
	transmissionMismatch,
	laneShutdown,
	endOfLane,
	fault,
	noMemory,
	noHardwareSupport,
	hardwareBroken,
};

} // namespace thor
