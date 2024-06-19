#pragma once

#include <assert.h>
#include <functional>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>
#include <format>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <optional>

namespace core::netlink {

struct Packet {
	// Sender netlink socket information.
	uint32_t senderPort = 0;
	uint32_t group = 0;

	// Sender process information.
	uint32_t senderPid = 0;

	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	size_t offset = 0;
};

struct NetlinkFile {
	virtual void deliver(core::netlink::Packet packet) = 0;
};

struct Group {
	friend struct NetlinkFile;

	// Sends a copy of the given message to this group.
	void carbonCopy(const core::netlink::Packet &packet) {
		for(auto socket : subscriptions)
			socket->deliver(packet);
	}

	std::vector<NetlinkFile *> subscriptions;
};

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
	inline void reset() {
		_packet = {};
		_offset = 0;
	}

	inline void group(uint32_t groupid) {
		_packet.group = groupid;
	}

	inline void header(uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
		assert(_offset == 0);
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
	inline void nlattr(uint8_t type, T data) {
		struct nlattr attr;
		attr.nla_type = type;
		attr.nla_len = NLA_HDRLEN + sizeof(T);
		size_t aligned_size = NLA_ALIGN(attr.nla_len);

		assert((_offset & (NLA_ALIGNTO - 1)) == 0);
		_packet.buffer.resize(_offset + aligned_size);
		assert(_packet.buffer.cbegin() + _offset + aligned_size <= _packet.buffer.cend());

		memcpy(_packet.buffer.data() + _offset, &attr, sizeof(struct nlattr));
		memcpy(_packet.buffer.data() + _offset + NLA_HDRLEN, &data, sizeof(T));
		_offset += aligned_size;

		buffer_align();
	};

	template<>
	inline void nlattr<std::string>(uint8_t type, std::string data) {
		size_t str_len = data.length() + 1;

		struct nlattr attr;
		attr.nla_type = type;
		attr.nla_len = NLA_HDRLEN + str_len;
		size_t aligned_size = NLA_ALIGN(attr.nla_len);

		assert((_offset & (NLA_ALIGNTO - 1)) == 0);
		_packet.buffer.resize(_offset + aligned_size);
		assert(_packet.buffer.cbegin() + _offset + aligned_size <= _packet.buffer.cend());

		memcpy(_packet.buffer.data() + _offset, &attr, sizeof(struct nlattr));
		memcpy(_packet.buffer.data() + _offset + NLA_HDRLEN, data.c_str(), str_len);
		_offset += aligned_size;

		buffer_align();
	};

	/**
	 * Set up a new nlattr that holds nested nlattrs, as created by the callback `cb`.
	 */
	template<typename T>
	inline void nested_nlattr(uint8_t type, std::function<void(NetlinkBuilder &, T)> cb, T ctx) {
		/* resize the buffer to nold our new nlattr header */
		_packet.buffer.resize(_offset + NLA_HDRLEN);
		/* use placement new to setting up the nlattr */
		struct nlattr *new_attr = new (_packet.buffer.data() + _offset) (struct nlattr);
		/* save the offset at the start of our new nlattr */
		auto prev_offset = _offset;
		new_attr->nla_type = type;
		/* adjust the nlattr header size, so that the callback starts writing past it */
		_offset += NLA_HDRLEN;

		cb(*this, ctx);

		/* the callback is very likely to resize the buffer, thus invalidating `new_attr` */
		auto nested_attr = reinterpret_cast<struct nlattr *>(_packet.buffer.data() + prev_offset);
		/* set the correct size: the difference between the offsets includes NLA_HDRLEN + whatever `cb` wrote */
		nested_attr->nla_len = (_offset - prev_offset);
		/* ensure we're still aligned properly */
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

	template<>
	inline void rtattr(uint8_t type, std::string data) {
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

	inline Packet packet(size_t sub = 0) {
		size_t size = _offset - sub;

		auto hdr = reinterpret_cast<nlmsghdr *>(_packet.buffer.data());
		hdr->nlmsg_len = size;

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

	Packet _packet;
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
	struct genl{};
}

inline std::optional<NetlinkAttrs<struct ifaddrmsg>> netlinkAttr(struct nlmsghdr *hdr, struct nl::packets::ifaddr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct ifaddrmsg>(hdr, msg, IFA_RTA(msg));
}

inline std::optional<NetlinkAttrs<struct ifinfomsg>> netlinkAttr(struct nlmsghdr *hdr, struct nl::packets::ifinfo) {
	const struct ifinfomsg *msg;

	if(auto opt = netlinkMessage<struct ifinfomsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct ifinfomsg>(hdr, msg, IFLA_RTA(msg));
}

inline std::optional<NetlinkAttrs<struct rtmsg>> netlinkAttr(struct nlmsghdr *hdr, struct nl::packets::rt) {
	const struct rtmsg *msg;

	if(auto opt = netlinkMessage<struct rtmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct rtmsg>(hdr, msg, RTM_RTA(msg));
}

inline std::optional<NetlinkAttrs<struct genlmsghdr>> netlinkAttr(struct nlmsghdr *hdr, struct nl::packets::genl) {
	const struct genlmsghdr *msg = nullptr;

	if(auto opt = netlinkMessage<struct genlmsghdr>(hdr, hdr->nlmsg_len)) {
		msg = *opt;
	} else {
		return std::nullopt;
	}

	return NetlinkAttrs<struct genlmsghdr>(hdr, msg, reinterpret_cast<struct rtattr *>(uintptr_t(msg) + GENL_HDRLEN));
}

inline void sendDone(NetlinkFile *f, struct nlmsghdr *hdr, struct sockaddr_nl *sa = nullptr) {
	NetlinkBuilder b;

	b.header(NLMSG_DONE, 0, hdr->nlmsg_seq, (sa != nullptr) ? sa->nl_pid : 0);
	b.message<uint32_t>(0);

	f->deliver(b.packet());
}

inline void sendError(NetlinkFile *f, struct nlmsghdr *hdr, int err, struct sockaddr_nl *sa = nullptr) {
	NetlinkBuilder b;

	b.header(NLMSG_ERROR, 0, hdr->nlmsg_seq, (sa != nullptr) ? sa->nl_pid : 0);

	b.message<struct nlmsgerr>({
		.error = -err,
		.msg = *hdr,
	});

	f->deliver(b.packet());
}

inline void sendAck(NetlinkFile *f, struct nlmsghdr *hdr) {
	NetlinkBuilder b;

	b.header(NLMSG_ERROR, NLM_F_CAPPED, hdr->nlmsg_seq, 0);
	b.message<struct nlmsgerr>({
		.error = 0,
		.msg = *hdr,
	});

	f->deliver(b.packet());
}

} // namespace core::netlink

/* enable dumping netlink packets with std::format */
template<>
struct std::formatter<core::netlink::Packet> : std::formatter<string_view> {
	auto format(const core::netlink::Packet& obj, std::format_context& ctx) const {
		std::string temp;

		for(auto c : obj.buffer) {
			std::format_to(std::back_inserter(temp), "\\x{:02x}", (unsigned char) c);
		}

		return std::formatter<string_view>::format(temp, ctx);
	}
};
