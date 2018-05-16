#ifndef THOR_GENERIC_ACCESSOR_HPP
#define THOR_GENERIC_ACCESSOR_HPP

#include "../arch/x86/paging.hpp"

namespace thor {

struct AddressSpace;

struct ForeignSpaceAccessor {
	// TODO: Use the constructor instead.
	static ForeignSpaceAccessor acquire(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length) {
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return ForeignSpaceAccessor(frigg::move(space), (uintptr_t)address, length);
	}

	friend void swap(ForeignSpaceAccessor &a, ForeignSpaceAccessor &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
	}

	ForeignSpaceAccessor() = default;
	
	ForeignSpaceAccessor(frigg::SharedPtr<AddressSpace> space,
			uintptr_t address, size_t length)
	: _space(frigg::move(space)), _address((void *)address), _length(length) { }

	ForeignSpaceAccessor(const ForeignSpaceAccessor &other) = delete;

	ForeignSpaceAccessor(ForeignSpaceAccessor &&other)
	: ForeignSpaceAccessor() {
		swap(*this, other);
	}
	
	ForeignSpaceAccessor &operator= (ForeignSpaceAccessor other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	uintptr_t address() {
		return (uintptr_t)_address;
	}
	size_t length() {
		return _length;
	}

	void load(size_t offset, void *pointer, size_t size);
	Error write(size_t offset, const void *pointer, size_t size);

	template<typename T>
	T read(size_t offset) {
		T value;
		load(offset, &value, sizeof(T));
		return value;
	}

	template<typename T>
	Error write(size_t offset, T value) {
		return write(offset, &value, sizeof(T));
	}

private:
	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
	size_t _length;
};

// directly accesses an object in an arbitrary address space.
// requires the object's address to be naturally aligned
// so that the object cannot cross a page boundary.
// requires the object to be smaller than a page for the same reason.
template<typename T>
struct DirectSpaceAccessor {
	DirectSpaceAccessor() = default;

	DirectSpaceAccessor(ForeignSpaceAccessor &lock, ptrdiff_t offset);

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
		frigg::swap(a._address, b._address);
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

	frigg::SharedPtr<AddressSpace> _space;
	T *_address;
};

struct KernelAccessor {
	static KernelAccessor acquire(void *pointer, size_t length) {
		return KernelAccessor(pointer, length);
	}

	friend void swap(KernelAccessor &a, KernelAccessor &b) {
		frigg::swap(a._pointer, b._pointer);
		frigg::swap(a._length, b._length);
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
		return kErrSuccess;
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
	AnyBufferAccessor(KernelAccessor accessor)
	: _variant(frigg::move(accessor)) { }
	
	AnyBufferAccessor(ForeignSpaceAccessor accessor)
	: _variant(frigg::move(accessor)) { }

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
	frigg::Variant<
		KernelAccessor,
		ForeignSpaceAccessor
	> _variant;
};

} // namespace thor

#endif // THOR_GENERIC_ACCESSOR_HPP
