#pragma once

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>

namespace common::uart {

template <typename Space>
struct Ns16550 {
	explicit Ns16550(Space regs);

	Ns16550(const Ns16550 &) = delete;
	Ns16550 &operator=(const Ns16550 &) = delete;

	void write(char c);

private:
	Space regs_;
};

extern template struct Ns16550<arch::mem_space>;
extern template struct Ns16550<arch::io_space>;

} // namespace common::uart
