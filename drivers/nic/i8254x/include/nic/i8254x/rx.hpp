#pragma once

#include <async/basic.hpp>
#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <arch/mem_space.hpp>
#include <arch/variable.hpp>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/queue.hpp>
#include <hel.h>
#include <stddef.h>
#include <stdint.h>
#include <queue>

namespace flags::rx::status {

constexpr arch::field<uint8_t, bool> done{0, 1};
constexpr arch::field<uint8_t, bool> end_of_packet{1, 1};

}

struct Intel8254xNic;
struct Request;

struct RxDescriptor {
	volatile uint64_t address;
	volatile uint16_t length;
	volatile uint16_t checksum;
	arch::bit_value<uint8_t> status{0};
	volatile uint8_t errors;
	volatile uint16_t special;
} __attribute__((packed));

static_assert(sizeof(RxDescriptor) == 16, "RxDescriptor should be 16 bytes");

struct RxQueue {
	friend Intel8254xNic;
private:
	RxQueue(size_t descriptors, Intel8254xNic &nic);

public:
	void ackAll();

	/**
	 * Return the physical address to the base of the descriptors
	 */
	uintptr_t getBase();

	/**
	 * Return the number of descriptors this queue can hold.
	 */
	size_t descriptors() {
		return _descriptor_count;
	}

	/**
	 * Return the length of the descriptor area in bytes.
	 */
	size_t getLength() {
		return _descriptor_count * sizeof(RxDescriptor);
	}

	/**
	 * Holds the index to the first descriptor to be written back into on packet reception by hardware.
	 **/
	QueueIndex head();
	QueueIndex tail();
	void tail(QueueIndex i);

	bool empty() {
		return head() == tail();
	}

	async::result<void> submitDescriptor(arch::dma_buffer_view frame, Intel8254xNic &nic);
	async::result<void> postDescriptor(arch::dma_buffer_view frame, Intel8254xNic &nic, Request *req, async::detached (*complete)(Request *));

private:
	Intel8254xNic &_nic;
	arch::dma_array<RxDescriptor> _descriptors;
	arch::dma_array<DescriptorSpace> _descriptor_buffers;
	std::queue<Request *> _requests;
	size_t _descriptor_count;
	QueueIndex next_index;
};
