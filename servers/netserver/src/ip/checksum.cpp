#include "checksum.hpp"

#include <arch/bit.hpp>

void Checksum::update(uint16_t word)  {
	state_ += word;
}

void Checksum::update(void *data, size_t size) {
	using namespace arch;
	auto iter = static_cast<unsigned char*>(data);
	if (size % 2 != 0) {
		iter--;
		update(iter[size - 1] << 8);
	}
	auto end = iter + size;
	for (; iter < end; iter += 2) {
		update(iter[0] << 8 | iter[1]);
	}
}

void Checksum::update(arch::dma_buffer_view view) {
	update(view.data(), view.size());
}

uint16_t Checksum::finalize() {
	auto state_ = this->state_;
	while (state_ >> 16 != 0) {
		state_ = (state_ >> 16) + (state_ & 0xffff);
	}
	return ~state_;
}
