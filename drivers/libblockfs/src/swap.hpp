#pragma once

#include <memory>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/common.hpp>

#include <blockfs.hpp>

namespace blockfs {

struct SwapTarget {
	virtual ~SwapTarget() = default;

	virtual arch::contiguous_pool *getSwapPool() = 0;
	virtual async::result<uint64_t> getSwapSize() = 0;

	virtual async::result<std::expected<void, Error>> enableSwap() = 0;

	virtual async::result<std::expected<void, Error>>
	readSwap(uint64_t offset, arch::dma_buffer_view) = 0;

	virtual async::result<std::expected<void, Error>>
	writeSwap(uint64_t offset, arch::dma_buffer_view) = 0;
};

// Announces a swap target.
async::result<protocols::fs::Error> swapon(std::shared_ptr<SwapTarget>);

} // namespace blockfs
