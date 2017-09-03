#ifndef THOR_GENERIC_ERROR_HPP
#define THOR_GENERIC_ERROR_HPP

namespace thor {

enum Error {
	kErrSuccess,
	kErrBufferTooSmall,
	kErrThreadExited,
	kErrClosedLocally,
	kErrClosedRemotely,
	kErrFault
};

} // namespace thor

#endif // THOR_GENERIC_ERROR_HPP
