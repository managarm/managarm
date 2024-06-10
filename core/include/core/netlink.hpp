#pragma once

#include <format>
#include <stddef.h>
#include <stdint.h>
#include <vector>

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
