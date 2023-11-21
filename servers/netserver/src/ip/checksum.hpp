#pragma once

#include <core/nic/buffer.hpp>
#include <stdint.h>

// 16-bit one's compliment sum checksum, as described in RFC791, amongst others
struct Checksum {
	void update(uint16_t word);
	void update(const void *mem, size_t size);
	void update(nic_core::buffer_view area);
	uint16_t finalize();

private:
	uint32_t state_ = 0;
};
