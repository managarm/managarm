#pragma once

#include <async/result.hpp>
#include <async/oneshot-event.hpp>
#include <arch/dma_structs.hpp>
#include <arch/variable.hpp>
#include <core/queue.hpp>
#include <stddef.h>
#include <stdint.h>

struct Descriptor {
	arch::bit_value<uint32_t> flags{0};
	uint32_t vlan;
	uint32_t base_low;
	uint32_t base_high;
};

static_assert(sizeof(Descriptor) == 16);

struct Request {
	Request(size_t size) : index(0, size) { };

	QueueIndex index;
	async::oneshot_event event;
	arch::dma_buffer_view frame;
};

namespace flags {

namespace tx {
	constexpr arch::field<uint32_t, bool> ownership(31, 1);
	constexpr bool owner_nic = true;
	constexpr bool owner_driver = false;
	constexpr arch::field<uint32_t, bool> eor(30, 1);
	constexpr arch::field<uint32_t, bool> first_segment(29, 1);
	constexpr arch::field<uint32_t, bool> last_segment(28, 1);
	constexpr arch::field<uint32_t, uint16_t> frame_length(0, 16);
}

namespace rx {
	constexpr arch::field<uint32_t, bool> ownership(31, 1);
	constexpr bool owner_nic = true;
	constexpr bool owner_driver = false;
	constexpr arch::field<uint32_t, bool> eor(30, 1);
	constexpr arch::field<uint32_t, bool> first_segment(29, 1);
	constexpr arch::field<uint32_t, bool> last_segment(28, 1);
	constexpr arch::field<uint32_t, bool> physical_address_ok(26, 1);
	constexpr arch::field<uint32_t, bool> broadcast_packet(25, 1);
	constexpr arch::field<uint32_t, bool> receive_watchdog_timer_expired(22, 1);
	constexpr arch::field<uint32_t, bool> receive_error(21, 1);
	constexpr arch::field<uint32_t, uint8_t> protocol_id(17, 2);
	constexpr arch::field<uint32_t, uint16_t> frame_length(0, 13);
}

}
