#ifndef POSIX_SUBSYSTEM_COMMON_HPP
#define POSIX_SUBSYSTEM_COMMON_HPP

#include <iostream>

#include <helix/ipc.hpp>

// TODO: remove this file

struct StructName {
	static StructName get(const char *type) {
		static uint64_t idCounter = 1;
		return StructName{type, idCounter++};
	}

	friend std::ostream &operator<< (std::ostream &os, const StructName &sn) {
		os << sn._type << "." << sn._id;
		return os;
	}

private:
	explicit StructName(const char *type, uint64_t id)
	: _type{type}, _id{id} { }

	const char *_type;
	uint64_t _id;
};

#endif // POSIX_SUBSYSTEM_COMMON_HPP
