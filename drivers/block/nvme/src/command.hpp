#pragma once

#include <async/result.hpp>
#include <async/promise.hpp>
#include <frg/std_compat.hpp>
#include <arch/dma_structs.hpp>

#include "spec.hpp"

#include <vector>

struct Command {
	using Result = std::pair<spec::CompletionStatus, spec::CompletionEntry::Result>;

	spec::Command &getCommandBuffer() {
		return command_;
	}

	void setupBuffer(arch::dma_buffer_view view);

	async::future<Result, frg::stl_allocator> getFuture() {
		return promise_.get_future();
	}

	void complete(spec::CompletionStatus status, spec::CompletionEntry::Result result) {
		promise_.set_value(Result{status, result});
	}

private:
	spec::Command command_;
	async::promise<Result, frg::stl_allocator> promise_;
	std::vector<arch::dma_array<uint64_t>> prpLists;
};
