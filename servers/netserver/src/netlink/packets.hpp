#pragma once

#include "netlink.hpp"
#include <linux/netlink.h>

template<typename T>
std::optional<const T *> netlinkMessage(const struct nlmsghdr *header, int length) {
	if(!NLMSG_OK(header, static_cast<uint32_t>(length)))
		return std::nullopt;

	if(length <= 0 || static_cast<uint32_t>(length) < (NLMSG_HDRLEN + sizeof(T)))
		return std::nullopt;
	return reinterpret_cast<const T*>(NLMSG_DATA(header));
}

/**
 * A utility for building up Netlink messages.
 */
struct NetlinkBuilder {
	inline void header(uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
		struct nlmsghdr hdr {
			.nlmsg_type = type,
			.nlmsg_flags = flags,
			.nlmsg_seq = seq,
			.nlmsg_pid = pid,
		};

		_packet.buffer.resize(_offset + sizeof(struct nlmsghdr));
		memcpy(_packet.buffer.data() + _offset, &hdr, sizeof(struct nlmsghdr));
		_offset += sizeof(struct nlmsghdr);

		buffer_align();
	}

	template<typename T>
	inline void message(T msg) {
		_packet.buffer.resize(_offset + sizeof(T));
		memcpy(_packet.buffer.data() + _offset, &msg, sizeof(T));
		_offset += sizeof(T);

		buffer_align();
	}

	template<typename T>
	inline void rtattr(uint8_t type, T data) {
		struct rtattr attr;
		attr.rta_type = type;
		attr.rta_len = RTA_LENGTH(sizeof(T));

		assert((_offset & (RTA_ALIGNTO - 1)) == 0);
		_packet.buffer.resize(_offset + attr.rta_len);
		assert(_packet.buffer.cbegin() + _offset + attr.rta_len <= _packet.buffer.cend());

		memcpy(_packet.buffer.data() + _offset, &attr, sizeof(struct rtattr));
		memcpy(_packet.buffer.data() + _offset + sizeof(struct rtattr), &data, sizeof(T));
		_offset += attr.rta_len;

		buffer_align();
	};

	inline void rtattr_string(uint8_t type, std::string data) {
		const size_t str_len = data.length() + 1;

		struct rtattr attr;
		attr.rta_type = type;
		attr.rta_len = RTA_LENGTH(str_len);

		_packet.buffer.resize(_offset + attr.rta_len);
		memcpy(_packet.buffer.data() + _offset, &attr, sizeof(struct rtattr));
		memcpy(_packet.buffer.data() + _offset + sizeof(struct rtattr), data.c_str(), str_len);
		_offset += attr.rta_len;

		buffer_align();
	};

	inline nl::Packet packet() {
		memcpy(_packet.buffer.data(), &_offset, sizeof(uint32_t));
		return std::move(_packet);
	}
private:
	/**
	 * Align the buffer to the netlink message alignment.
	 */
	inline void buffer_align() {
		auto size = NLMSG_ALIGN(_offset); // NLMSG_ALIGN only aligns up
		if(_offset != size) {
			_packet.buffer.resize(size);
			memset(_packet.buffer.data() + _offset, 0, size - _offset);
		}
		_offset = size;
	};

	nl::Packet _packet;
	size_t _offset = 0;
};
