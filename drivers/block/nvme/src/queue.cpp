#include <arch/bit.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include "queue.hpp"
#include "spec.hpp"

Queue::Queue(unsigned int qid, unsigned int depth, arch::mem_space doorbells)
	: qid_(qid), depth_(depth), doorbells_(doorbells), sqTail_(0), cqHead_(0), cqPhase_(1) {
	queuedCmds_.resize(depth);
}

void Queue::init() {
	auto align = 0x1000;
	size_t sqSize = ((depth_ << 6) + align - 1) & ~size_t(align - 1);
	size_t cqSize = ((depth_ * sizeof(spec::CompletionEntry)) + align - 1) & ~size_t(align - 1);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(cqSize, kHelAllocContinuous, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
						   0, cqSize, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));

	cqes_ = reinterpret_cast<spec::CompletionEntry *>(window);
	memset(cqes_, 0, cqSize);

	HEL_CHECK(helAllocateMemory(sqSize, kHelAllocContinuous, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
						   0, sqSize, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));

	sqCmds_ = window;

	cqPhys_ = helix::ptrToPhysical(cqes_);
	sqPhys_ = helix::ptrToPhysical(sqCmds_);
}

async::detached Queue::run() {
	submitPendingLoop();

	co_return;
}

int Queue::handleIrq() {
	using arch::convert_endian;
	using arch::endian;

	int found = 0;
	spec::CompletionEntry *cqe = &cqes_[cqHead_];

	while ((convert_endian<endian::little>(cqe->status) & 1) == cqPhase_) {
		found++;

		auto status = convert_endian<endian::little>(cqe->status) >> 1;
		auto slot = cqe->commandId;
		assert(slot < queuedCmds_.size());
		assert(queuedCmds_[slot]);

		std::unique_ptr<Command> cmd = std::move(queuedCmds_[slot]);
		cmd->complete(status, cqe->result);

		if (++cqHead_ == depth_) {
			cqHead_ = 0;
			cqPhase_ ^= 1;
		}

		cqe = &cqes_[cqHead_];
	}

	if (commandsInFlight_ == depth_ && found > 0)
		freeSlotDoorbell_.raise();

	commandsInFlight_ -= found;

	if (found)
		doorbells_.store(arch::scalar_register<uint32_t>{0x4}, cqHead_);

	return found;
}

async::result<size_t> Queue::findFreeSlot() {
	if (commandsInFlight_ >= depth_)
		co_await freeSlotDoorbell_.async_wait();

	for (size_t i = 0; i < queuedCmds_.size(); i++) {
		if (!queuedCmds_[i]) {
			co_return i;
		}
	}

	assert(!"Command queue is full.");
	co_return 0;
}

async::detached Queue::submitPendingLoop() {
	while (true) {
		auto cmd = co_await pendingCmdQueue_.async_get();
		assert(cmd);
		co_await submitCommandToDevice(std::move(cmd.value()));
	}
}

async::result<void> Queue::submitCommandToDevice(std::unique_ptr<Command> cmd) {
	auto slot = co_await findFreeSlot();

	auto &cmdBuf = cmd->getCommandBuffer();
	cmdBuf.common.commandId = (uint16_t)slot;

	memcpy((uint8_t *)sqCmds_ + (sqTail_ << 6), &cmdBuf, sizeof(spec::Command));
	if (++sqTail_ == depth_)
		sqTail_ = 0;
	doorbells_.store(arch::scalar_register<uint32_t>{0}, sqTail_);

	queuedCmds_[slot] = std::move(cmd);
	commandsInFlight_++;
}

async::result<Command::Result> Queue::submitCommand(std::unique_ptr<Command> cmd) {
	auto future = cmd->getFuture();

	pendingCmdQueue_.put(std::move(cmd));
	return future;
}
