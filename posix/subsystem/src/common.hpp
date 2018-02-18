
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>

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

