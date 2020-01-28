#ifndef THOR_GENERIC_ERROR_HPP
#define THOR_GENERIC_ERROR_HPP

namespace thor {

enum Error {
	kErrSuccess,
	kErrIllegalArgs,
	kErrIllegalObject,
	kErrIllegalState,
	kErrCancelled,
	kErrBufferTooSmall,
	kErrThreadExited,
	kErrTransmissionMismatch,
	kErrLaneShutdown,
	kErrEndOfLane,
	kErrFault,
	kErrNoMemory
};

} // namespace thor

#endif // THOR_GENERIC_ERROR_HPP
