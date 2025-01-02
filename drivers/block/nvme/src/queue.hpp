#pragma once

#include <memory>

#include <arch/mem_space.hpp>
#include <async/recurring-event.hpp>
#include <async/queue.hpp>
#include <frg/std_compat.hpp>

#include "command.hpp"
#include "spec.hpp"

struct Queue {
	Queue(unsigned int index, unsigned int depth) : qid_{index}, depth_{depth} {
		queuedCmds_.resize(depth);
	};

	virtual ~Queue() = default;

	virtual async::result<void> init() = 0;
	virtual async::detached run() = 0;

	virtual async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) = 0;

	unsigned int getQueueId() const {
		return qid_;
	}
	unsigned int getQueueDepth() const {
		return depth_;
	}

	async::result<size_t> findFreeSlot();

protected:
	unsigned int qid_;
	unsigned int depth_;

	async::queue<std::unique_ptr<Command>, frg::stl_allocator> pendingCmdQueue_;
	std::vector<std::unique_ptr<Command>> queuedCmds_;
	async::recurring_event freeSlotDoorbell_;
	size_t commandsInFlight_ = 0;
};

struct PciExpressQueue final : Queue {
	PciExpressQueue(unsigned int index, unsigned int depth, arch::mem_space doorbells);

	async::result<void> init() override;
	async::detached run() override;

	uintptr_t getCqPhysAddr() const {
		return cqPhys_;
	}
	uintptr_t getSqPhysAddr() const {
		return sqPhys_;
	}

	int handleIrq();

private:
	arch::mem_space doorbells_;
	spec::CompletionEntry *cqes_;
	void *sqCmds_;
	uintptr_t cqPhys_;
	uintptr_t sqPhys_;
	uint16_t sqTail_;
	uint16_t cqHead_;
	uint8_t cqPhase_;

	async::detached submitPendingLoop();

	async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) override;
	async::result<void> submitCommandToDevice(std::unique_ptr<Command> cmd);
};
