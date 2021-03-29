#include <helix/memory.hpp>

#include <inttypes.h>

#include "port.hpp"
#include "util.hpp"

namespace regs {
	constexpr arch::scalar_register<uint32_t> clBase{0x0}; 
	constexpr arch::scalar_register<uint32_t> clBaseUpper{0x4}; 
	constexpr arch::scalar_register<uint32_t> fisBase{0x8}; 
	constexpr arch::scalar_register<uint32_t> fisBaseUpper{0xC}; 
	constexpr arch::scalar_register<uint32_t> interruptStatus{0x10}; 
	constexpr arch::scalar_register<uint32_t> interruptEnable{0x14}; 
	constexpr arch::scalar_register<uint32_t> commandAndStatus{0x18}; 
	constexpr arch::scalar_register<uint32_t> tfd{0x20}; 
	constexpr arch::scalar_register<uint32_t> status{0x28}; 
	constexpr arch::scalar_register<uint32_t> sErr{0x30};
	constexpr arch::scalar_register<uint32_t> commandIssue{0x38}; 
}

namespace flags {
	namespace cmd {
		constexpr int CMD_LIST_RUNNING	  = 1 << 15;
		constexpr int FIS_RECEIVE_RUNNING = 1 << 14;
		constexpr int FIS_RECEIVE_ENABLE  = 1 << 4;
		constexpr int SPIN_UP_DEVICE	  = 1 << 1;
		constexpr int START				  = 1;
	}

	namespace is {
		constexpr int TASK_FILE_ERROR	= 1 << 30;
		constexpr int HOST_FATAL_ERROR	= 1 << 29;
		constexpr int HOST_DATA_ERROR	= 1 << 28;
		constexpr int IF_FATAL_ERROR	= 1 << 27;
		constexpr int IF_NONFATAL_ERROR = 1 << 26;
		constexpr int D2H_FIS			= 1;
	}
}

namespace {
	constexpr size_t SECTOR_SIZE = 512;
	constexpr bool LOG_COMMANDS  = false;
}

// TODO: We can use a more appropriate block size, but this breaks other parts of the OS.
Port::Port(int portIndex, size_t numCommandSlots, arch::mem_space regs)
	: BlockDevice{SECTOR_SIZE},  regs_{regs}, numCommandSlots_{numCommandSlots},
	commandsInFlight_{0}, portIndex_{portIndex} {

}

async::result<bool> Port::init() {
	// Spin up device
	auto cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::SPIN_UP_DEVICE);

	// If PxSSTS.DET != 3, PxSSTS.IPM != 1, then ignore the device for now
	// TODO: We should poll a bit (how long?), and check for BSY/DRQ bits (10.3.1)
	auto status = regs_.load(regs::status);
	auto ipm = (status >> 8) & 0xF;
	auto det = status & 0xF;
	if (ipm != 1 && det != 3)
		co_return false;

	// 10.1.2, part 3:
	// Clear PxCMD.ST
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas & ~flags::cmd::START);

	// Wait until PxCMD.CR = 0 with 500ms timeout
	auto timedOut = co_await kindaBusyWait(500'000'000, [&](){
		return !(regs_.load(regs::commandAndStatus) & flags::cmd::CMD_LIST_RUNNING); });
	assert(!timedOut);
	
	// Clear PxCMD.FRE (must be done before rebase)
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas & ~flags::cmd::FIS_RECEIVE_ENABLE);

	// Wait until PxCMD.FR = 0 with 500ms timeout
	timedOut = co_await kindaBusyWait(500'000'000, [&](){
			return !(regs_.load(regs::commandAndStatus) & flags::cmd::FIS_RECEIVE_RUNNING); });
	assert(!timedOut);

	// Allocate memory for command list, received FIS, and command tables
	// Note: the combination of libarch DMA types and helPointerPhysical ensures that
	// these buffers will remain present in the page tables at all times.
	commandList_ = arch::dma_object<command_list>{nullptr};
	commandTables_ = arch::dma_array<command_table>{nullptr, numCommandSlots_};
	receivedFis_ = arch::dma_object<received_fis>{nullptr};

	uintptr_t clPhys = virtToPhys(commandList_.data()),
			  ctPhys = virtToPhys(&commandTables_[0]),
			  rfPhys = virtToPhys(receivedFis_.data());
	assert((clPhys & 0x3FF) == 0 && clPhys < std::numeric_limits<uint32_t>::max());
	assert((ctPhys & 0x7F) == 0 && ctPhys < std::numeric_limits<uint32_t>::max());
	assert((rfPhys & 0xFF) == 0 && rfPhys < std::numeric_limits<uint32_t>::max());

	regs_.store(regs::clBase, static_cast<uint32_t>(clPhys));
	regs_.store(regs::clBaseUpper, 0);
	regs_.store(regs::fisBase, static_cast<uint32_t>(rfPhys));
	regs_.store(regs::fisBaseUpper, 0);

	printf("block/ahci: Discovered port %d, ipm %x, det %x\n", portIndex_, ipm, det);

	co_return true;
}

async::detached Port::run() {
	// Enable FIS receive
	auto cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::FIS_RECEIVE_ENABLE);

	// Start port (10.3.1)
	assert(!(regs_.load(regs::commandAndStatus) & flags::cmd::CMD_LIST_RUNNING));
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::START);

	size_t slot = co_await findFreeSlot_();

	arch::dma_object<identify_device> identify{nullptr};
	Command cmd = Command(identify.data(), CommandType::identify);
	cmd.prepare(commandTables_[slot], commandList_->slots[slot]);

	regs_.store(regs::commandIssue, 1 << slot);

	// Just poll for completion for simplicity
	auto timedOut = co_await kindaBusyWait(500'000'000,
			[&](){ return !(regs_.load(regs::commandIssue) & (1 << slot)); });
	assert(!timedOut);

	assert(identify->supportsLba48());
	auto sectorCount = identify->maxLBA48;

	printf("block/ahci: Started port %d, sector count %" PRIu64 ", model %s\n",
			portIndex_, sectorCount, identify->getModel().c_str());

	// Clear errors
	regs_.store(regs::sErr, ~0);

	// Clear and enable interrupts on this port
	auto is = regs_.load(regs::interruptStatus);
	regs_.store(regs::interruptStatus, is);
	auto ie = regs_.load(regs::interruptEnable);
	regs_.store(regs::interruptEnable, ie
			| flags::is::D2H_FIS
			| flags::is::TASK_FILE_ERROR
			| flags::is::HOST_DATA_ERROR
			| flags::is::HOST_FATAL_ERROR
			| flags::is::IF_FATAL_ERROR
			| flags::is::IF_NONFATAL_ERROR);

	submitPendingLoop_();

	blockfs::runDevice(this);
	co_return;
}

async::result<size_t> Port::findFreeSlot_() {
	while (commandsInFlight_ >= numCommandSlots_) {
		if (LOG_COMMANDS) {
			printf("block/ahci: submission queue full, waiting...\n");
		}

		co_await freeSlotDoorbell_.async_wait();
	}

	// We can't look at CI here, as the HBA might clear it before we have
	// a chance to notify completion, so the array slot will still be occupied.
	// TODO: We could use a bitmask and CLZ for this.
	for (int i = 0; i < numCommandSlots_; i++) {
		if (!submittedCmds_[i]) {
			co_return i;
		}
	}

	assert(!"commandsInFlight < numCommandSlots, but submission queue was full");
	co_return 0;
}

void Port::handleIrq() {
	auto is = regs_.load(regs::interruptStatus);

	// Check errors
	// TODO: Make this more robust (log non-fatal errors, try to recover, print more state etc.)
	if (is & (flags::is::HOST_FATAL_ERROR | flags::is::IF_FATAL_ERROR)) {
		printf("\e[31mblock/ahci: Port %d encountered fatal error, PxIS = %u, PxSERR = %u\e[39m\n",
				portIndex_, is, regs_.load(regs::sErr));
		abort();
	}

	if (LOG_COMMANDS) {
		printf("block/ahci: Port %d handling IRQ: PxIS %x, PxIE %x, TFD %x, CI %x, CAS %x\n",
				portIndex_, is, regs_.load(regs::interruptEnable), regs_.load(regs::tfd),
				regs_.load(regs::commandIssue), regs_.load(regs::commandAndStatus));
	}

	// Notify all completed commands
	auto numCompleted = 0;
	auto cmdActiveMask = regs_.load(regs::commandIssue);
	for (int i = 0; i < numCommandSlots_; i++) {
		if (submittedCmds_[i] && !(cmdActiveMask & (1 << i))) {
			std::unique_ptr<Command> cmd = std::move(submittedCmds_[i]);
			cmd->notifyCompletion();
			numCompleted++;
		}
	}

	// If the buffer has gone from full to not full, wake the tasks waiting for a free slot.
	// TODO: If we have a lot of waiters, this will cause many spurious wakeups. Ideally, we only
	// notify a certain number of tasks, and the rest can stay asleep.
	if (commandsInFlight_ == numCommandSlots_ && numCompleted > 0) {
		freeSlotDoorbell_.ring();
	}

	commandsInFlight_ -= numCompleted;

	// Acknowledge the interrupt
	regs_.store(regs::interruptStatus, is);
}

async::detached Port::submitPendingLoop_() {
	while (true) {
		auto cmd =	co_await pendingCmdQueue_.async_get();
		assert(cmd);
		co_await submitCommand_(std::move(cmd.value()));
	}
}

async::result<void> Port::submitCommand_(std::unique_ptr<Command> cmd) {
	auto slot = co_await findFreeSlot_();
	assert(!(regs_.load(regs::commandIssue) & (1 << slot)));
	assert(!submittedCmds_[slot]);

	// Setup command table and FIS
	cmd->prepare(commandTables_[slot], commandList_->slots[slot]);

	// Issue command
	submittedCmds_[slot] = std::move(cmd);
	commandsInFlight_++;
	regs_.store(regs::commandIssue, 1 << slot);

	co_return;
}

async::result<void> Port::readSectors(uint64_t sector, void *buffer, size_t numSectors) {
	auto cmd = std::make_unique<Command>(sector, numSectors, numSectors * SECTOR_SIZE,
			buffer, CommandType::read);
	auto future = cmd->getFuture();

	pendingCmdQueue_.put(std::move(cmd));
	return future;
}

async::result<void> Port::writeSectors(uint64_t sector, const void *buffer, size_t numSectors) {
	auto cmd = std::make_unique<Command>(sector, numSectors, numSectors * SECTOR_SIZE,
			const_cast<void *>(buffer), CommandType::write);
	auto future = cmd->getFuture();

	pendingCmdQueue_.put(std::move(cmd));
	return future;
}
