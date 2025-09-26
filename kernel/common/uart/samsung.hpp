#pragma once

#include <arch/mem_space.hpp>

namespace common::uart {

struct Samsung {
	explicit Samsung(uintptr_t base);

	Samsung(const Samsung &) = delete;
	Samsung &operator=(const Samsung &) = delete;

	uintptr_t base() const { return base_; }

	void write(char c);

private:
	uintptr_t base_;
	arch::mem_space space_;
};

} // namespace common::uart
