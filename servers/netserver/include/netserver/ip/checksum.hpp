#pragma once

#include <arch/dma_structs.hpp>

// 16-bit one's compliment sum checksum, as described in RFC791, amongst others
struct Checksum {
	void update(uint16_t word);
	void update(void *mem, size_t size);
	void update(arch::dma_buffer_view area);
	uint16_t finalize();

private:
	uint32_t state = 0;
};
