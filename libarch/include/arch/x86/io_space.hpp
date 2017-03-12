
#ifndef LIBARCH_IO_SPACE_HPP
#define LIBARCH_IO_SPACE_HPP

#include <stdint.h>

#include <arch/register.hpp>

namespace arch {

namespace _detail {
	template<typename B>
	struct io_ops;

	template<>
	struct io_ops<uint8_t> {
		static void store(uint16_t addr, uint8_t v) {
			asm volatile ("outb %0, %1" : : "a"(v), "d"(addr) : "memory");
		}
		static uint8_t load(uint16_t addr) {
			uint8_t v;
			asm volatile ("inb %1, %0" : "=a"(v) : "d"(addr) : "memory");
			return v;
		}
	};

	template<>
	struct io_ops<uint16_t> {
		static void store(uint16_t addr, uint16_t v) {
			asm volatile ("outw %0, %1" : : "a"(v), "d"(addr) : "memory");
		}
		static uint16_t load(uint16_t addr) {
			uint16_t v;
			asm volatile ("inw %1, %0" : "=a"(v) : "d"(addr) : "memory");
			return v;
		}
	};

	template<>
	struct io_ops<uint32_t> {
		static void store(uint16_t addr, uint32_t v) {
			asm volatile ("outl %0, %1" : : "a"(v), "d"(addr) : "memory");
		}
		static uint32_t load(uint16_t addr) {
			uint32_t v;
			asm volatile ("inl %1, %0" : "=a"(v) : "d"(addr) : "memory");
			return v;
		}
	};

	struct io_space {
		constexpr io_space()
		: _base(0) { }

		constexpr io_space(uint16_t base)
		: _base(base) { }

		io_space subspace(ptrdiff_t offset) const {
			return io_space(_base + offset);
		}

		template<typename RT>
		void store(RT r, typename RT::rep_type value) const {
			auto v = static_cast<typename RT::bits_type>(value);
			io_ops<typename RT::bits_type>::store(_base + r.offset(), v);
		}

		template<typename RT>
		typename RT::rep_type load(RT r) const {
			auto b = io_ops<typename RT::bits_type>::load(_base + r.offset());
			return static_cast<typename RT::rep_type>(b);
		}

	private:
		uint16_t _base;
	};
}

using _detail::io_space;

static constexpr io_space global_io(0);

} // namespace arch

#endif // LIBARCH_IO_SPACE_HPP

