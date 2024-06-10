#pragma once

#include "netlink.hpp"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <optional>

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
		struct nlmsghdr hdr{};
		hdr.nlmsg_type = type;
		hdr.nlmsg_flags = flags;
		hdr.nlmsg_seq = seq;
		hdr.nlmsg_pid = pid;

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

	inline core::netlink::Packet packet() {
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

	core::netlink::Packet _packet;
	size_t _offset = 0;
};

/**
 * Helper class to retrieve `struct rtattr`s from a netlink message
 *
 * This class mostly serves to implement an Iterator for attributes. The Iterator returns instances of `Attr` to allow safe access to attributes.
 */
template<typename T>
struct NetlinkAttrs {
	explicit NetlinkAttrs(const struct nlmsghdr *hdr, const T *s, const struct rtattr *attrs) : _hdr{hdr}, _s{s}, _attrs{attrs} {};

	struct Iterator;

	/**
	 * Helper class to encapsulate a `struct rtattr` and allow safe access to its data.
	 */
	struct Attr {
		friend Iterator;

		/**
		 * Converting constructor from `struct rtattr`.
		 *
		 * This is not marked explicit to allow for implicit conversion.
		 */
		Attr(const struct rtattr *attr) : _attr{attr} { };

		/**
		 * Returns the `rta_type` of the attribute
		 */
		unsigned short type() const {
			return _attr->rta_type;
		}

		/**
		 * Type-safe and bounds-checked access to attribute data.
		 */
		template<typename D>
		std::optional<D> data() const {
			if(length() >= RTA_LENGTH(sizeof(D)))
				return { *reinterpret_cast<D *>(RTA_DATA(_attr)) };
			else
				return std::nullopt;
		}

		/**
		 * Return the rtattr data as a `std::string`.
		 */
		std::optional<std::string> str() const {
			/* Assert that there is even a string to begin with */
			if(length() < RTA_LENGTH(1))
				return std::nullopt;
			/* Assert that the string length actually matches the attr length */
			if(length() < RTA_LENGTH(strlen((const char *) RTA_DATA(_attr))))
				return std::nullopt;
			return std::string((const char *) RTA_DATA(_attr));
		}

	private:
		/**
		 * Return the length of this `struct rtattr`.
		 *
		 * As consumers should not care about the length of an attribute (as they should not do pointer arithmetic on this anyways), this is marked private.
		 */
		size_t length() const {
			return _attr->rta_len;
		}

		const struct rtattr *_attr;
	};

	struct Iterator {
		using iterator_category = std::forward_iterator_tag;
		using different_type = ptrdiff_t;
		using value_type = struct Attr;
		using pointer = value_type *;
		using reference = value_type &;

		Iterator(value_type val) : _val{val} { };

		Iterator &operator++() {
			size_t dummy = _val.length();
			_val = RTA_NEXT(_val._attr, dummy);
			return *this;
		}

		reference operator*() {
			return _val;
		}

		friend bool operator== (const Iterator& a, const Iterator& b) { return a._val._attr == b._val._attr; };
    	friend bool operator!= (const Iterator& a, const Iterator& b) { return a._val._attr != b._val._attr; };
	private:
		value_type _val;
	};

	Iterator begin() {
		if(_attrs)
			return Iterator{_attrs.value()};
		return end();
	}

	Iterator end() {
		auto ptr = uintptr_t(_hdr) + _hdr->nlmsg_len;
		return Iterator{reinterpret_cast<const struct rtattr *>(ptr)};
	}
private:
	const struct nlmsghdr *_hdr;
	const T *_s;
	std::optional<const struct rtattr *> _attrs = std::nullopt;
};

namespace nl::packets {
	struct ifaddr{};
	struct ifinfo{};
	struct rt{};
}

inline std::optional<NetlinkAttrs<struct ifaddrmsg>> NetlinkAttr(struct nlmsghdr *hdr, struct nl::packets::ifaddr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct ifaddrmsg>(hdr, msg, IFA_RTA(msg));
}

inline std::optional<NetlinkAttrs<struct ifinfomsg>> NetlinkAttr(struct nlmsghdr *hdr, struct nl::packets::ifinfo) {
	const struct ifinfomsg *msg;

	if(auto opt = netlinkMessage<struct ifinfomsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct ifinfomsg>(hdr, msg, IFLA_RTA(msg));
}

inline std::optional<NetlinkAttrs<struct rtmsg>> NetlinkAttr(struct nlmsghdr *hdr, struct nl::packets::rt) {
	const struct rtmsg *msg;

	if(auto opt = netlinkMessage<struct rtmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct rtmsg>(hdr, msg, RTM_RTA(msg));
}
