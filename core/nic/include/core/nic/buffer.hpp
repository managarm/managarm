#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <cstddef>
#include <helix/ipc.hpp>
#include <hel.h>
#include <protocols/mbus/client.hpp>

#include <iostream>

namespace nic_core {

// TODO: should these be in frigg?
struct buffer_owner {
	buffer_owner(size_t size, size_t offset = 0)
	: size{size}, _offset{offset} {
		assert(size);
		_allocatedSize = (size + 0x1000 - 1) & ~(0x1000 - 1);
		HEL_CHECK(helAllocateMemory(_allocatedSize, 0, nullptr, &_memory));
		HEL_CHECK(helMapMemory(_memory, kHelNullHandle, nullptr, _offset, _allocatedSize, kHelMapProtRead | kHelMapProtWrite, &data));
	}

	buffer_owner(HelHandle memory, size_t allocatedSize, size_t size, size_t offset)
	: size{size}, _offset{offset}, _allocatedSize{allocatedSize}, _memory{std::move(memory)}{
		assert(size);
		HEL_CHECK(helMapMemory(_memory, kHelNullHandle, nullptr, _offset, _allocatedSize, kHelMapProtRead | kHelMapProtWrite, &data));
	}

	// Unmap this memory and close the descriptor.
	~buffer_owner() {
		HEL_CHECK(helUnmapMemory(kHelNullHandle, data, _allocatedSize));
		HEL_CHECK(helCloseDescriptor(kHelThisUniverse, _memory));
	}

	void *data;
	size_t size;

	HelHandle getHandle() const {
		return _memory;
	}

private:
	// These are used to unmap the memory when the buffer_owner gets destroyed.
	size_t _offset;
	size_t _allocatedSize;
	HelHandle _memory;
};


struct buffer_view {
	buffer_view()
	: _data{nullptr}, _size{0}, _offset{0}, _buffer{nullptr} {}

	buffer_view(std::shared_ptr<buffer_owner> buffer)
	: _data{buffer->data}, _size{buffer->size}, _offset{0}, _buffer{buffer} { }

	~buffer_view() = default;
	
	static buffer_view fromHelHandle(HelHandle memory, size_t allocatedSize, size_t length, size_t offset = 0);

	constexpr void *data() const {
		return _data;
	}

	constexpr std::byte *byte_data() const {
		return static_cast<std::byte *>(_data);
	}

	constexpr size_t size() const {
		return _size;
	}

	buffer_view subview(size_t offset, size_t chunk) const {
		assert(offset <= _size);
		assert(offset + chunk <= _size);
		return buffer_view{_offset + offset, chunk, _buffer};
	}

	buffer_view subview(size_t offset) const {
		assert(offset <= _size);
		return buffer_view{_offset + offset, _size - offset, _buffer};
	}

	HelHandle copyHandle(size_t &allocatedSize) const {
		HelHandle new_handle;
		allocatedSize = (_size + 0x1000 - 1) & ~(0x1000 - 1);
		HEL_CHECK(helCopyOnWrite(_buffer->getHandle(), _offset, _size, &new_handle));
		return new_handle;
	}

private:
	buffer_view(size_t offset, size_t size, std::shared_ptr<buffer_owner> buffer)
	: _size{size}, _offset{offset}, _buffer{buffer} {
		_data = (char*)(_buffer->data) + _offset;
		assert(_data <= ((char*)(_buffer->data) + _buffer->size));
	}

	void *_data;
	size_t _size;

	// This keeps the memory mapped until the last buffer_view is destroyed.
	// We store the offset so that we can quickly copy the handle from the buffer_owner.
	size_t _offset;
	std::shared_ptr<buffer_owner> _buffer;
};

} // namespace nic_core
