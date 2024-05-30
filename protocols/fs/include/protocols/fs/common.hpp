#pragma once

#include <optional>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <variant>
#include <vector>

#include "fs.bragi.hpp"

namespace protocols {
namespace fs {

enum class Error {
	none = 0,
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
	noSpaceLeft = 21,
	noBackingDevice = 23,
	isDirectory = 22,
	directoryNotEmpty = 24,
	invalidProtocolOption = 25,
};

inline managarm::fs::Errors mapFsError(Error e) {
	switch(e) {
		case Error::none: return managarm::fs::Errors::SUCCESS;
		case Error::fileNotFound: return managarm::fs::Errors::FILE_NOT_FOUND;
		case Error::endOfFile: return managarm::fs::Errors::END_OF_FILE;
		case Error::illegalArguments: return managarm::fs::Errors::ILLEGAL_ARGUMENT;
		case Error::wouldBlock: return managarm::fs::Errors::WOULD_BLOCK;
		case Error::seekOnPipe: return managarm::fs::Errors::SEEK_ON_PIPE;
		case Error::brokenPipe: return managarm::fs::Errors::BROKEN_PIPE;
		case Error::accessDenied: return managarm::fs::Errors::ACCESS_DENIED;
		case Error::notDirectory: return managarm::fs::Errors::NOT_DIRECTORY;
		case Error::afNotSupported: return managarm::fs::Errors::AF_NOT_SUPPORTED;
		case Error::destAddrRequired: return managarm::fs::Errors::DESTINATION_ADDRESS_REQUIRED;
		case Error::netUnreachable: return managarm::fs::Errors::NETWORK_UNREACHABLE;
		case Error::messageSize: return managarm::fs::Errors::MESSAGE_TOO_LARGE;
		case Error::hostUnreachable: return managarm::fs::Errors::HOST_UNREACHABLE;
		case Error::insufficientPermissions: return managarm::fs::Errors::INSUFFICIENT_PERMISSIONS;
		case Error::addressInUse: return managarm::fs::Errors::ADDRESS_IN_USE;
		case Error::addressNotAvailable: return managarm::fs::Errors::ADDRESS_NOT_AVAILABLE;
		case Error::notConnected: return managarm::fs::Errors::NOT_CONNECTED;
		case Error::alreadyExists: return managarm::fs::Errors::ALREADY_EXISTS;
		case Error::illegalOperationTarget: return managarm::fs::Errors::ILLEGAL_OPERATION_TARGET;
		case Error::noSpaceLeft: return managarm::fs::Errors::NO_SPACE_LEFT;
		case Error::noBackingDevice: return managarm::fs::Errors::NO_BACKING_DEVICE;
		case Error::isDirectory: return managarm::fs::Errors::IS_DIRECTORY;
		case Error::invalidProtocolOption: return managarm::fs::Errors::INVALID_PROTOCOL_OPTION;
		case Error::directoryNotEmpty: return managarm::fs::Errors::DIRECTORY_NOT_EMPTY;
	}
}

using ReadResult = std::variant<Error, size_t>;

using ReadEntriesResult = std::optional<std::string>;

using PollResult = std::tuple<uint64_t, int, int>;
using PollWaitResult = std::tuple<uint64_t, int>;
using PollStatusResult = std::tuple<uint64_t, int>;

struct RecvData {
	std::vector<char> ctrl;
	size_t dataLength;
	size_t addressLength;
	uint32_t flags;
};

using RecvResult = std::variant<Error, RecvData>;
using SendResult = std::variant<Error, size_t>;

struct CtrlBuilder {
	CtrlBuilder(size_t max_size)
	: _maxSize{max_size}, _offset{0} { }

	bool message(int layer, int type, size_t payload) {
		if(_buffer.size() + CMSG_SPACE(payload) > _maxSize)
			return false;

		_offset = _buffer.size();
		_buffer.resize(_offset + CMSG_SPACE(payload));

		struct cmsghdr h;
		memset(&h, 0, sizeof(struct cmsghdr));
		h.cmsg_len = CMSG_LEN(payload);
		h.cmsg_level = layer;
		h.cmsg_type = type;

		memcpy(_buffer.data() + _offset, &h, sizeof(struct cmsghdr));
		_offset += CMSG_ALIGN(sizeof(struct cmsghdr));

		return true;
	}

	template<typename T>
	void write(T data) {
		memcpy(_buffer.data() + _offset, &data, sizeof(T));
		_offset += sizeof(T);
	}

	std::vector<char> buffer() {
		return std::move(_buffer);
	}

private:
	std::vector<char> _buffer;
	size_t _maxSize;
	size_t _offset;
};

} } // namespace protocols::fs
