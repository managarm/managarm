#ifndef LIBFS_COMMON_HPP
#define LIBFS_COMMON_HPP

#include <optional>
#include <variant>
#include <vector>

namespace protocols {
namespace fs {

enum class Error {
	none = 0,
	illegalRequest = 3,
	fileNotFound = 1,
	endOfFile = 2,
	illegalArguments = 4,
	wouldBlock = 5,
	seekOnPipe = 6,
	brokenPipe = 7,
	accessDenied = 8,
	notDirectory = 20,

	afNotSupported = 9,
	destAddrRequired = 10,
	netUnreachable = 11,
	messageSize = 12,
	hostUnreachable = 13,
	insufficientPermissions = 14,
	addressInUse = 15,
	addressNotAvailable = 16,
	notConnected = 17,
	alreadyExists = 18,
	illegalOperationTarget = 19,
	noSpaceLeft = 20,
};

using ReadResult = std::variant<Error, size_t>;

using ReadEntriesResult = std::optional<std::string>;

using PollResult = std::tuple<uint64_t, int, int>;
using PollWaitResult = std::tuple<uint64_t, int>;
using PollStatusResult = std::tuple<uint64_t, int>;

struct RecvData {
	size_t dataLength;
	size_t addressLength;
	std::vector<char> ctrl;
};

using RecvResult = std::variant<Error, RecvData>;
using SendResult = std::variant<Error, size_t>;

} } // namespace protocols::fs

#endif // LIBFS_COMMON_HPP
