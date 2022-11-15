#pragma once

#include <async/basic.hpp>
#include <async/oneshot-event.hpp>
#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <arch/mem_space.hpp>
#include <arch/variable.hpp>
#include <hel.h>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/queue.hpp>
#include <stddef.h>
#include <stdint.h>
#include <queue>

struct Intel8254xNic;
struct Request;

namespace flags::tx {

namespace cmd {

constexpr arch::field<uint8_t, bool> end_of_packet{0, 1};
constexpr arch::field<uint8_t, bool> insert_fcs{1, 1};
constexpr arch::field<uint8_t, bool> report_status{3, 1};

}

namespace status {

constexpr arch::field<uint8_t, bool> done{0, 1};
constexpr arch::field<uint8_t, bool> end_of_packet{1, 1};

}

} // namespace flags::tx

struct TxDescriptor {
	volatile uint64_t address;
	volatile uint16_t length;
	volatile uint8_t cso;
	arch::bit_value<uint8_t> cmd{0};
	arch::bit_value<uint8_t> status{0};
	volatile uint8_t css;
	volatile uint16_t special;
};

static_assert(sizeof(TxDescriptor) == 16, "TxDescriptor should be 16 bytes");

struct TxQueue {
	friend Intel8254xNic;
private:
	TxQueue(size_t descriptors, Intel8254xNic &nic);

public:
	/**
	 * Return the physical address to the base of the descriptors
	 */
	uintptr_t getBase();
	void *getDescriptorPtr(size_t index);

	size_t descriptors() {
		return _descriptor_count;
	}

	size_t getLength() {
		return _descriptor_count * sizeof(TxDescriptor);
	}

	QueueIndex head();
	QueueIndex tail();

private:
	Intel8254xNic &_nic;
	arch::dma_array<TxDescriptor> _descriptors;
	arch::dma_array<DescriptorSpace> _descriptor_buffers;
	std::queue<Request *> _requests;
	size_t _descriptor_count;
};
