#pragma once

#include <arch/mem_space.hpp>
#include <async/recurring-event.hpp>
#include <async/queue.hpp>

#include "command.hpp"
#include "spec.hpp"

class Queue {
public:
    Queue(unsigned int index, unsigned int depth, arch::mem_space doorbells);

    void init();
    async::detached run();

    unsigned int getQueueId() const { return  qid_; }
    unsigned int getQueueDepth() const { return  depth_; }

    uintptr_t getCqPhysAddr() const { return cqPhys_; }
    uintptr_t getSqPhysAddr() const { return sqPhys_; }

    async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd);

    int handleIrq();

private:
    unsigned int qid_;
    unsigned int depth_;
    arch::mem_space doorbells_;
    spec::CompletionEntry* cqes_;
    void* sqCmds_;
    uintptr_t cqPhys_;
    uintptr_t sqPhys_;
    uint16_t sqTail_;
    uint16_t cqHead_;
    uint8_t cqPhase_;

        struct stl_allocator {
        void *allocate(size_t size) {
            return operator new(size);
        }

        void deallocate(void *p, size_t) {
            return operator delete(p);
        }
    };
    async::queue<std::unique_ptr<Command>, stl_allocator> pendingCmdQueue_;

    std::vector<std::unique_ptr<Command>> queuedCmds_;
    async::recurring_event freeSlotDoorbell_;
    size_t commandsInFlight_;

    async::result<size_t> findFreeSlot();
    async::detached submitPendingLoop();

    async::result<void> submitCommandToDevice(std::unique_ptr<Command> cmd);
};
