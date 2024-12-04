#pragma once

#include <array>
#include <elf.h>

#define THOR_DEFINE_ELF_NOTE(name) \
	[[gnu::section(".note.managarm"), gnu::used]] constinit decltype(name) name

namespace thor {

// Name must be an std::array holding a null-terminated string.
template<auto Name, typename T>
struct ElfNote {
	static_assert(alignof(T) <= 8);

	constexpr ElfNote(unsigned int type, T data)
	: hdr_{Name.size() - 1, sizeof(T), type}, name_{}, data_{data} {
		for (size_t i = 0; i < Name.size() - 1; ++i)
			name_[i] = Name[i];
	}

	T *operator-> () {
		return &data_;
	}

private:
	Elf64_Nhdr hdr_;
	char name_[Name.size()];
	alignas(8) T data_;
};

template<typename T>
using ManagarmElfNote = ElfNote<std::to_array("Managarm"), T>;

} // namespace thor
