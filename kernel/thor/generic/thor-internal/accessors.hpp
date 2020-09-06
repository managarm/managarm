#pragma once

#include <frg/variant.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/paging.hpp>

namespace thor {

// directly accesses an object in an arbitrary address space.
// requires the object's address to be naturally aligned
// so that the object cannot cross a page boundary.
// requires the object to be smaller than a page for the same reason.
template<typename T>
struct DirectSpaceAccessor {
	DirectSpaceAccessor() = default;

	DirectSpaceAccessor(AddressSpaceLockHandle &lock, ptrdiff_t offset);

	T *get() {
		return reinterpret_cast<T *>((char *)_accessor.get() + _misalign);
	}

private:
	PageAccessor _accessor;
	ptrdiff_t _misalign;
};

template<typename T>
struct DirectSelfAccessor {
	static DirectSelfAccessor acquire(T *address) {
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return DirectSelfAccessor(address);
	}

	friend void swap(DirectSelfAccessor &a, DirectSelfAccessor &b) {
		using std::swap;
		swap(a._address, b._address);
	}

	DirectSelfAccessor()
	: _address(nullptr) { }

	DirectSelfAccessor(const DirectSelfAccessor &other) = delete;

	DirectSelfAccessor(DirectSelfAccessor &&other)
	: DirectSelfAccessor() {
		swap(*this, other);
	}
	
	DirectSelfAccessor &operator= (DirectSelfAccessor other) {
		swap(*this, other);
		return *this;
	}
	
	T *get() {
		assert(_address);
		return _address;
	}

	T &operator* () {
		return *get();
	}
	T *operator-> () {
		return get();
	}

private:
	DirectSelfAccessor(T *address)
	: _address(address) { }

	smarter::shared_ptr<AddressSpace, BindableHandle> _space;
	T *_address;
};

struct KernelAccessor {
	static KernelAccessor acquire(void *pointer, size_t length) {
		return KernelAccessor(pointer, length);
	}

	friend void swap(KernelAccessor &a, KernelAccessor &b) {
		using std::swap;
		swap(a._pointer, b._pointer);
		swap(a._length, b._length);
	}

	KernelAccessor() = default;

	KernelAccessor(const KernelAccessor &other) = delete;

	KernelAccessor(KernelAccessor &&other)
	: KernelAccessor() {
		swap(*this, other);
	}
	
	KernelAccessor &operator= (KernelAccessor other) {
		swap(*this, other);
		return *this;
	}

	size_t length() {
		return _length;
	}

	Error write(size_t offset, void *source, size_t size) {
		// TODO: detect overflows here.
		assert(offset + size <= _length);
		memcpy((char *)_pointer + offset, source, size);
		return Error::success;
	}

private:
	KernelAccessor(void *pointer, size_t length)
	: _pointer(pointer), _length(length) { }

private:
	void *_pointer;
	size_t _length;
};

struct AnyBufferAccessor {
public:
	AnyBufferAccessor() { }

	AnyBufferAccessor(KernelAccessor accessor)
	: _variant(std::move(accessor)) { }
	
	AnyBufferAccessor(AddressSpaceLockHandle accessor)
	: _variant(std::move(accessor)) { }

	size_t length() {
		return _variant.apply([&] (auto &accessor) -> size_t {
			return accessor.length();
		});
	}

	Error write(size_t offset, void *source, size_t size) {
		return _variant.apply([&] (auto &accessor) -> Error {
			return accessor.write(offset, source, size);
		});
	}

private:
	frg::variant<
		KernelAccessor,
		AddressSpaceLockHandle
	> _variant;
};

} // namespace thor
