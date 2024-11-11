#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <stddef.h>
#include <span>
#include <optional>

struct KernelLogs {
	async::result<size_t> getMessage(std::span<uint8_t> buffer);

private:
	async::result<void> setupKerncfg();

	size_t offset_ = 0;
	std::optional<helix::UniqueDescriptor> kerncfgLane_ = std::nullopt;
};
