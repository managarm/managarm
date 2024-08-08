#pragma once

#include <iostream>
#include <format>
#include <unordered_map>

#include <helix/ipc.hpp>

// TODO: remove this file

struct StructName {
	static StructName get(const char *type) {
		static uint64_t idCounter = 1;
		return StructName{type, idCounter++};
	}

	std::string str() const {
		return std::format("{}.{}", _type, _id);
	}

	friend std::ostream &operator<< (std::ostream &os, const StructName &sn) {
		os << sn.str();
		return os;
	}

private:
	explicit StructName(const char *type, uint64_t id)
	: _type{type}, _id{id} { }

	const char *_type;
	uint64_t _id;
};

template <>
struct std::formatter<StructName> : std::formatter<std::string> {
	auto format(const StructName& obj, std::format_context& ctx) const {
		return std::formatter<std::string>::format(obj.str(), ctx);
	}
};
