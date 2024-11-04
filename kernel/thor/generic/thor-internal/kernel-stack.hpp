#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

namespace thor {

struct StackBase {
	void *sp;
};

struct UniqueKernelStack {
	static constexpr size_t kSize = 0xF000;

	static UniqueKernelStack make();

	friend void swap(UniqueKernelStack &a, UniqueKernelStack &b) {
		using std::swap;
		swap(a._base, b._base);
	}

	UniqueKernelStack() : _base(nullptr) {}

	UniqueKernelStack(const UniqueKernelStack &other) = delete;

	UniqueKernelStack(UniqueKernelStack &&other) : UniqueKernelStack() { swap(*this, other); }

	~UniqueKernelStack();

	UniqueKernelStack &operator=(UniqueKernelStack other) {
		swap(*this, other);
		return *this;
	}

	StackBase base() { return StackBase{_base}; }
	void *basePtr() { return _base; }

	template <typename T, typename... Args> T *embed(Args &&...args) {
		// TODO: Do not use a magic number as stack alignment here.
		_base -= (sizeof(T) + 15) & ~size_t{15};
		return new (_base) T{std::forward<Args>(args)...};
	}

	bool contains(void *sp) {
		return uintptr_t(sp) >= uintptr_t(_base) + kSize && uintptr_t(sp) <= uintptr_t(_base);
	}

  private:
	explicit UniqueKernelStack(char *base) : _base(base) {}

	char *_base;
};

} // namespace thor
