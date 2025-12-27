#pragma once

#include <async/oneshot-event.hpp>
#include <arch/dma_structs.hpp>
#include <core/queue.hpp>
#include <stddef.h>

struct Request {
	async::oneshot_primitive event;
	arch::dma_buffer_view frame;
	size_t size;
};
