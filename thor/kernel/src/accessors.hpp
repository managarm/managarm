
namespace thor {

// directly accesses an object in an arbitrary address space.
// requires the object's address to be naturally aligned
// so that the object cannot cross a page boundary.
// requires the object to be smaller than a page for the same reason.
template<typename T>
struct DirectSpaceLock {
	static DirectSpaceLock acquire(frigg::SharedPtr<AddressSpace> space, T *address) {
		assert(sizeof(T) <= kPageSize);
		assert((VirtualAddr)address % sizeof(T) == 0);
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return DirectSpaceLock(frigg::move(space), address);
	}

	friend void swap(DirectSpaceLock &a, DirectSpaceLock &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
	}

	DirectSpaceLock() = default;

	DirectSpaceLock(const DirectSpaceLock &other) = delete;

	DirectSpaceLock(DirectSpaceLock &&other)
	: DirectSpaceLock() {
		swap(*this, other);
	}
	
	DirectSpaceLock &operator= (DirectSpaceLock other) {
		swap(*this, other);
		return *this;
	}
	
	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	void *foreignAddress() {
		return _address;
	}

	T *get();

	T &operator* () {
		return *get();
	}
	T *operator-> () {
		return get();
	}

private:
	DirectSpaceLock(frigg::SharedPtr<AddressSpace> space, T *address)
	: _space(frigg::move(space)), _address(address) { }

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
};

struct ForeignSpaceLock {
	static ForeignSpaceLock acquire(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length) {
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return ForeignSpaceLock(frigg::move(space), address, length);
	}

	friend void swap(ForeignSpaceLock &a, ForeignSpaceLock &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
	}

	ForeignSpaceLock() = default;

	ForeignSpaceLock(const ForeignSpaceLock &other) = delete;

	ForeignSpaceLock(ForeignSpaceLock &&other)
	: ForeignSpaceLock() {
		swap(*this, other);
	}
	
	ForeignSpaceLock &operator= (ForeignSpaceLock other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	size_t length() {
		return _length;
	}

	void copyTo(void *pointer, size_t size);

private:
	ForeignSpaceLock(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length)
	: _space(frigg::move(space)), _address(address), _length(length) { }

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
	size_t _length;
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

} // namespace thor

