#pragma once

#include <async/result.hpp>
#include <arch/dma_structs.hpp>

#include "spec.hpp"

class Command {
public:
    using Result = std::pair<uint16_t, spec::CompletionEntry::Result>;

    spec::Command& getCommandBuffer() { return  command_; }

    void setupBuffer(arch::dma_buffer_view view);

    async::result<Result> getFuture() {
        return promise_.async_get();
    }

    void complete(uint16_t status, spec::CompletionEntry::Result result) {
        promise_.set_value(Result{status, result});
    }

private:
    spec::Command command_;
    async::promise<Result> promise_;
    std::vector<arch::dma_array<uint64_t>> prpLists;
};
