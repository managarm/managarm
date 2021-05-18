#include <inttypes.h>

#include <helix/memory.hpp>
#include <helix/timer.hpp>

#include "port.hpp"

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
		constexpr int iccActive         = 1 << 28;
		constexpr int cmdListRunning    = 1 << 15;
		constexpr int fisReceiveRunning = 1 << 14;
		constexpr int fisReceiveEnable  = 1 << 4;
		constexpr int spinUpDevice      = 1 << 1;
		constexpr int start             = 1;
	}

	namespace is {
		constexpr int taskFileError   = 1 << 30;
		constexpr int hostFatalError  = 1 << 29;
		constexpr int hostDataError   = 1 << 28;
		constexpr int ifFatalError    = 1 << 27;
		constexpr int ifNonFatalError = 1 << 26;
		constexpr int d2hFis          = 1;
	}

	namespace tfd {
		constexpr int bsy = 1 << 7;
		constexpr int drq = 1 << 3;
	}
}

namespace {
	constexpr size_t sectorSize = 512;
	constexpr bool logCommands  = false;
}

// TODO: We can use a more appropriate block size, but this breaks other parts of the OS.
Port::Port(int portIndex, size_t numCommandSlots, bool staggeredSpinUp, arch::mem_space regs)
	: BlockDevice{::sectorSize},  regs_{regs}, numCommandSlots_{numCommandSlots},
	commandsInFlight_{0}, portIndex_{portIndex}, staggeredSpinUp_{staggeredSpinUp} {

}

async::result<bool> Port::init() {
	if (staggeredSpinUp_) {
		// Spin up device
		auto cas = regs_.load(regs::commandAndStatus);
		regs_.store(regs::commandAndStatus, cas | flags::cmd::spinUpDevice);

		// Wait up to 10ms for PxSSTS.DET = 1 or 3 (AHCI spec: 10.1.1, SATA 3.2 spec: 17.7.2)
		auto success = co_await helix::kindaBusyWait(10'000'000, [&]() {
			auto det = regs_.load(regs::status) & 0xF;
			return det == 1 || det == 3;
		});
		if (!success) {
			printf("block/ahci: Couldn't spin up port %d\n", portIndex_);
			co_return false;
		}
	}

	// Set link to active
	auto cas = regs_.load(regs::commandAndStatus);
	cas &= ~(0xF << 28); // Clear ICC
	regs_.store(regs::commandAndStatus, cas | flags::cmd::iccActive);

	// If PxSSTS.DET != 3, PxSSTS.IPM != 1 at this point, then ignore the device for now
	auto status = regs_.load(regs::status);
	auto ipm = (status >> 8) & 0xF;
	auto det = status & 0xF;
	if (ipm != 1 && det != 3)
		co_return false;

	// 10.1.2, part 3:
	// Clear PxCMD.ST
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas & ~flags::cmd::start);

	// Wait until PxCMD.CR = 0 with 500ms timeout
	auto success = co_await helix::kindaBusyWait(500'000'000, [&](){
		return !(regs_.load(regs::commandAndStatus) & flags::cmd::cmdListRunning); });
	assert(success);
	
	// Clear PxCMD.FRE (must be done before rebase)
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas & ~flags::cmd::fisReceiveEnable);

	// Wait until PxCMD.FR = 0 with 500ms timeout
	success = co_await helix::kindaBusyWait(500'000'000, [&](){
			return !(regs_.load(regs::commandAndStatus) & flags::cmd::fisReceiveRunning); });
	assert(success);

	// Allocate memory for command list, received FIS, and command tables
	// Note: the combination of libarch DMA types and ptrToPhysical ensures that
	// these buffers will remain present in the page tables at all times.
	commandList_ = arch::dma_object<commandList>{nullptr};
	commandTables_ = arch::dma_array<commandTable>{nullptr, numCommandSlots_};
	receivedFis_ = arch::dma_object<receivedFis>{nullptr};

	uintptr_t clPhys = helix::ptrToPhysical(commandList_.data()),
			  ctPhys = helix::ptrToPhysical(&commandTables_[0]),
			  rfPhys = helix::ptrToPhysical(receivedFis_.data());
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
	regs_.store(regs::commandAndStatus, cas | flags::cmd::fisReceiveEnable);

	// Check that the BSY and DRQ bits are clear (necessary as per 10.3.1)
	auto tfd = regs_.load(regs::tfd);
	if ((tfd & flags::tfd::bsy) || (tfd & flags::tfd::drq)) {
		printf("block/ahci: Failed to start busy port %d\n", portIndex_);
		co_return;
	}

	// Start port (10.3.1)
	assert(!(regs_.load(regs::commandAndStatus) & flags::cmd::cmdListRunning));
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::start);

	size_t slot = co_await findFreeSlot_();

	arch::dma_object<identifyDevice> identify{nullptr};
	Command cmd = Command(identify.data(), CommandType::identify);
	cmd.prepare(commandTables_[slot], commandList_->slots[slot]);

	regs_.store(regs::commandIssue, 1 << slot);

	// Just poll for completion for simplicity
	auto success = co_await helix::kindaBusyWait(500'000'000,
			[&](){ return !(regs_.load(regs::commandIssue) & (1 << slot)); });
	assert(success);

	assert(identify->supportsLba48());
	auto [logicalSize, physicalSize] = identify->getSectorSize();
	auto sectorCount = identify->maxLBA48;
	auto model = identify->getModel();

	printf("block/ahci: Started port %d, model %s, logical sector size %zu, "
			"physical sector size %zu, sector count %" PRIu64 "\n",
			portIndex_, model.c_str(), logicalSize, physicalSize, sectorCount);
	assert(logicalSize == 512 && "block/ahci: logical sector size > 512 is not supported");

	// Clear errors
	regs_.store(regs::sErr, ~0);

	// Clear and enable interrupts on this port
	auto is = regs_.load(regs::interruptStatus);
	regs_.store(regs::interruptStatus, is);
	auto ie = regs_.load(regs::interruptEnable);
	regs_.store(regs::interruptEnable, ie
			| flags::is::d2hFis
			| flags::is::taskFileError
			| flags::is::hostDataError
			| flags::is::hostFatalError
			| flags::is::ifFatalError
			| flags::is::ifNonFatalError);

	submitPendingLoop_();

	blockfs::runDevice(this);
	co_return;
}

async::result<size_t> Port::findFreeSlot_() {
	while (commandsInFlight_ >= numCommandSlots_) {
		if (logCommands) {
			printf("block/ahci: submission queue full, waiting...\n");
		}

		co_await freeSlotDoorbell_.async_wait();
	}

	// We can't look at CI here, as the HBA might clear it before we have
	// a chance to notify completion, so the array slot will still be occupied.
	// TODO: We could use a bitmask and CLZ for this.
	for (size_t i = 0; i < numCommandSlots_; i++) {
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
	if (is & (flags::is::hostFatalError | flags::is::ifFatalError)) {
		printf("\e[31mblock/ahci: Port %d encountered fatal error, PxIS = %u, PxSERR = %u\e[39m\n",
				portIndex_, is, regs_.load(regs::sErr));
		abort();
	}

	if (logCommands) {
		printf("block/ahci: Port %d handling IRQ: PxIS %x, PxIE %x, TFD %x, CI %x, CAS %x\n",
				portIndex_, is, regs_.load(regs::interruptEnable), regs_.load(regs::tfd),
				regs_.load(regs::commandIssue), regs_.load(regs::commandAndStatus));
	}

	// Notify all completed commands
	auto numCompleted = 0;
	auto cmdActiveMask = regs_.load(regs::commandIssue);
	for (size_t i = 0; i < numCommandSlots_; i++) {
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
		freeSlotDoorbell_.raise();
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
	auto cmd = std::make_unique<Command>(sector, numSectors, numSectors * sectorSize,
			buffer, CommandType::read);
	auto future = cmd->getFuture();

	pendingCmdQueue_.put(std::move(cmd));
	return future;
}

async::result<void> Port::writeSectors(uint64_t sector, const void *buffer, size_t numSectors) {
	auto cmd = std::make_unique<Command>(sector, numSectors, numSectors * sectorSize,
			const_cast<void *>(buffer), CommandType::write);
	auto future = cmd->getFuture();

	pendingCmdQueue_.put(std::move(cmd));
	return future;
}
