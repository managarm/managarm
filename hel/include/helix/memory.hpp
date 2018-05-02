#ifndef HELIX_MEMORY_HPP
#define HELIX_MEMORY_HPP

#include <string.h>

#include <hel.h>
#include <helix/ipc.hpp>

namespace helix {

struct Mapping {
	static constexpr size_t pageSize = 0x1000;

	friend void swap(Mapping &x, Mapping &y) {
		using std::swap;
		swap(x._window, y._window);
		swap(x._offset, y._offset);
		swap(x._size, y._size);
	}

	Mapping()
	: _window{nullptr}, _offset{0}, _size{0} { }

	Mapping(helix::BorrowedDescriptor memory, ptrdiff_t offset, size_t size)
	: _offset{offset}, _size{size} {
		HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle,
				nullptr, _offset & ~(pageSize - 1),
				((_offset & (pageSize - 1)) + _size + (pageSize - 1)) & ~(pageSize - 1),
				kHelMapProtRead | kHelMapProtWrite, &_window));
	}

	Mapping(const Mapping &) = delete;
	
	Mapping(Mapping &&other)
	: Mapping() {
		swap(*this, other);
	}

	~Mapping() {
		if(_window) {
			auto aligned_size = (_size + (pageSize - 1)) & ~(pageSize - 1);
			HEL_CHECK(helUnmapMemory(kHelNullHandle, _window, aligned_size));
		}
	}

	Mapping &operator= (Mapping other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _window;
	}

	void *get() {
		return reinterpret_cast<char *>(_window) + (_offset & (pageSize - 1));
	}

private:
	void *_window;
	ptrdiff_t _offset;
	size_t _size;
};

} // namespace helix

#endif // HEL_MEMORY_HPP
