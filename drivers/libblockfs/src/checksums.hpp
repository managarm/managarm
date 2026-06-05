#pragma once

#include <cstdint>
#include <cstddef>
#include <array>

namespace blockfs::checksums {

// CRC algorithms adapted from https://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks

struct Crc32c {
	Crc32c(uint32_t seed) : value_{seed} { }

	void addData(const void *data, size_t size) {
		auto ptr = static_cast<const uint8_t *>(data);
		for(size_t i = 0; i < size; i++) {
			value_ ^= ptr[i];
			value_ = (value_ >> 8) ^ crc32cTable[value_ & 0xff];
		}
	}

	uint32_t finalize() const {
		return value_;
	}

private:
	static constexpr auto crc32cTable = [] {
		std::array<uint32_t, 0x100> arr{};

		uint32_t crc32 = 1;
		for(uint32_t i = 128; i != 0; i >>= 1) {
			crc32 = (crc32 >> 1) ^ (crc32 & 1 ? 0x82F63B78 : 0);

			for(uint32_t j = 0; j < 256; j += 2 * i)
				arr[i + j] = crc32 ^ arr[j];
		}

		return arr;
	}();

	uint32_t value_;
};

struct Crc16 {
	Crc16(uint16_t seed) : value_{seed} { }

	void addData(const void *data, size_t size) {
		auto ptr = static_cast<const uint8_t *>(data);
		for(size_t i = 0; i < size; i++) {
			value_ ^= ptr[i];
			value_ = (value_ >> 8) ^ crc16Table[value_ & 0xff];
		}
	}

	uint16_t finalize() const {
		return value_;
	}

private:
	static constexpr auto crc16Table = [] {
		std::array<uint16_t, 0x100> arr{};

		uint16_t crc16 = 1;
		for(uint32_t i = 128; i != 0; i >>= 1) {
			crc16 = (crc16 >> 1) ^ (crc16 & 1 ? 0xA001 : 0);

			for(uint32_t j = 0; j < 256; j += 2 * i)
				arr[i + j] = crc16 ^ arr[j];
		}

		return arr;
	}();

	uint16_t value_;
};

} // namespace blockfs::checksums
