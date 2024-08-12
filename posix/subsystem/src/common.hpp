#pragma once

#include <iostream>
#include <format>
#include <iterator>
#include <unordered_map>

#include <helix/ipc.hpp>

// TODO: remove this file

struct StructName {
	static StructName get(const char *type) {
		static uint64_t idCounter = 1;
		return StructName{type, idCounter++};
	}

	friend std::ostream &operator<< (std::ostream &os, const StructName &sn) {
		std::format_to(std::ostream_iterator<char>(os), "{}.{}", sn._type, sn._id);
		return os;
	}

	friend struct std::formatter<StructName>;

private:
	explicit StructName(const char *type, uint64_t id)
	: _type{type}, _id{id} { }

	const char *_type;
	uint64_t _id;
};

template <>
struct std::formatter<StructName> : std::formatter<std::string> {
	auto format(const StructName& obj, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "{}.{}", obj._type, obj._id);
	}
};
