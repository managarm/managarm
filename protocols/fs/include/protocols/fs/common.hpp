#pragma once

#include <optional>
#include <variant>
#include <vector>
#include <sys/socket.h>

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

		memcpy(_buffer.data(), &h, sizeof(struct cmsghdr));
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
