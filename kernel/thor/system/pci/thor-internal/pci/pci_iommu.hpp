#pragma once

#include <arch/bits.hpp>
#include <arch/variable.hpp>

#include <stdint.h>

namespace thor::pci {

namespace requestIDMasks {
	constexpr arch::field<uint16_t, uint8_t> function{0, 3};
	constexpr arch::field<uint16_t, uint8_t> device{3, 5};
	constexpr arch::field<uint16_t, uint8_t> bus{8, 8};
}

struct RequestID {
	operator uint16_t() {
		return uint16_t{data_.load()};
	}

	uint8_t bus() {
		return data_.load() & requestIDMasks::bus;
	}

	uint8_t device() {
		return data_.load() & requestIDMasks::device;
	}

	uint8_t function() {
		return data_.load() & requestIDMasks::function;
	}

	uint8_t devfn() {
		return (device() << 3) | function();
	}

	RequestID(uint8_t bus, uint8_t slot, uint8_t function)
	: data_{requestIDMasks::function(function)
			| requestIDMasks::device(slot) | requestIDMasks::bus(bus)} {
	}

	explicit RequestID(uint16_t val) :
	data_{arch::bit_value{val}} { }

private:
	arch::bit_variable<uint16_t> data_;
};
static_assert(sizeof(RequestID) == 2);

} // namespace thor::pci
