#pragma once

#include <arch/mem_space.hpp>

namespace common::uart {

struct PL011 {
	PL011(uintptr_t base, uint64_t clock);

	PL011(const PL011 &) = delete;
	PL011 &operator=(const PL011 &) = delete;

	uintptr_t base() const { return base_; }

	void init(uint64_t baud);
	void disable();

	void write(char c);

private:
	uintptr_t base_;
	arch::mem_space space_;
	uint64_t clock_;
};

} // namespace common::uart
