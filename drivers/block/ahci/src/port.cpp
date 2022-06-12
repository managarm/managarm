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
	constexpr arch::scalar_register<uint32_t> sataControl{0x2C};
	constexpr arch::scalar_register<uint32_t> sErr{0x30};
	constexpr arch::scalar_register<uint32_t> sataActive{0x34};
	constexpr arch::scalar_register<uint32_t> commandIssue{0x38};
}

namespace flags {
	namespace cmd {
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
}

// TODO: We can use a more appropriate block size, but this breaks other parts of the OS.
Port::Port(int64_t parentId, int portIndex, size_t numCommandSlots, bool staggeredSpinUp, arch::mem_space regs)
	: BlockDevice{::sectorSize, parentId},  regs_{regs}, deviceSize_{0},
	numCommandSlots_{numCommandSlots}, commandsInFlight_{0}, portIndex_{portIndex}, 
	staggeredSpinUp_{staggeredSpinUp}
{
}

async::result<bool> Port::init() {
	// If PxSSTS.DET != 3, PxSSTS.IPM != 1 at this point, then ignore the device for now
	auto status = regs_.load(regs::status);
	auto ipm = (status >> 8) & 0xF;
	auto det = status & 0xF;
	if (ipm != 1 && det != 3)
		co_return false;

	// 10.1.2, part 3:
	// Clear PxCMD.ST
	auto cas = regs_.load(regs::commandAndStatus);
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

	if (staggeredSpinUp_) {
		// Spin up device
		auto cas = regs_.load(regs::commandAndStatus);
		regs_.store(regs::commandAndStatus, cas | flags::cmd::spinUpDevice);

		// Wait up to 10ms for PxSSTS.DET = 1 or 3 (AHCI spec: 10.1.1, SATA 3.2 spec: 17.7.2)
		auto success = co_await helix::kindaBusyWait(10'000'000, [&]() {
			auto det = regs_.load(regs::status) & 0xF;
			// return det == 1 || det == 3;
			return det == 3;
		});
		if (!success) {
			printf("block/ahci: Couldn't spin up port %d\n", portIndex_);
			co_return false;
		}
	}

	// TODO: If the port isn't available here, we may try a COMRESET (AHCI spec 10.4.2)

	status = regs_.load(regs::status);
	ipm = (status >> 8) & 0xF;
	det = status & 0xF;
	printf("block/ahci: Discovered port %d, PxSSTS.IPM %#x, PxSSTS.DET %#x\n", portIndex_, ipm, det);

	co_return true;
}

void Port::dumpState() {
	printf("block/ahci: Dumping port %d state:\n", portIndex_);
	printf("  PxSERR: %#x\n", regs_.load(regs::sErr));
	printf("  PxCMD: %#x\n", regs_.load(regs::commandAndStatus));
	printf("  PxCI: %#x\n", regs_.load(regs::commandIssue));
	printf("  PxTFD: %#x\n", regs_.load(regs::tfd));
	printf("  PxSSTS: %#x\n", regs_.load(regs::status));
	printf("  PxSCTL: %#x\n", regs_.load(regs::sataControl));
	printf("  PxSACT: %#x\n", regs_.load(regs::sataActive));
	printf("  PxIS: %#x\n", regs_.load(regs::interruptStatus));
	printf("  PxIE: %#x\n", regs_.load(regs::interruptEnable));
	printf("  commandsInFlight: %zu\n", commandsInFlight_);
	printf("  submittedCmds slots used: %zu\n", std::count_if(submittedCmds_.begin(), submittedCmds_.end(), [](auto &p){ return p != nullptr; }));
}

// Start port (10.3.1).
async::result<bool> Port::run() {
	printf("block/ahci: Starting port %d\n", portIndex_);

	// Clear errors
	regs_.store(regs::sErr, regs_.load(regs::sErr));

	// Allocate memory for command list, received FIS, and command tables.
	// arch::dma_* structs should guarantee that these are always present in memory,
	// and are physically contiguous.
	commandList_ = arch::dma_object<commandList>{&dmaPool_};
	commandTables_ = arch::dma_array<commandTable>{&dmaPool_, numCommandSlots_};
	receivedFis_ = arch::dma_object<receivedFis>{&dmaPool_};

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

	// Enable FIS receive
	auto cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::fisReceiveEnable);

	// Check that the BSY and DRQ bits are clear (necessary as per 10.3.1)
	auto success = co_await helix::kindaBusyWait(10'000'000'000, [&](){
		auto tfd = regs_.load(regs::tfd);
		return (tfd & flags::tfd::bsy) == 0 && (tfd & flags::tfd::drq) == 0;
	});
	if (!success) {
		printf("\e[31mblock/ahci: Failed to start busy port %d\e[39m\n", portIndex_);
		dumpState();
		co_return false;
	}

	// Set PxCMD.ST
	assert(!(regs_.load(regs::commandAndStatus) & flags::cmd::cmdListRunning));
	cas = regs_.load(regs::commandAndStatus);
	regs_.store(regs::commandAndStatus, cas | flags::cmd::start);

	size_t slot = co_await findFreeSlot_();

	arch::dma_object<identifyDevice> identify{&dmaPool_};
	Command cmd = Command(identify.data(), CommandType::identify);
	cmd.prepare(commandTables_[slot], commandList_->slots[slot]);

	regs_.store(regs::commandIssue, 1 << slot);

	// For simplicity, poll for completion (500ms)
	success = co_await helix::kindaBusyWait(500'000'000,
			[&](){ return !(regs_.load(regs::commandIssue) & (1 << slot)); });
	if (!success) {
		printf("\e[31mblock/ahci: Port %d identify failed\n", portIndex_);
		dumpState();
		co_return false;
	}

	assert(identify->supportsLba48());
	auto [logicalSize, physicalSize] = identify->getSectorSize();
	auto sectorCount = identify->maxLBA48;
	auto model = identify->getModel();
	deviceSize_ = logicalSize * sectorCount;

	printf("block/ahci: Started port %d, model %s, size %.1fGiB (sectors: logical %zu, physical %zu, count %" PRIu64 ")\n",
			portIndex_, model.c_str(), static_cast<float>(deviceSize_ / (1 << 30)),
			logicalSize, physicalSize, sectorCount);
	assert(logicalSize == 512 && "block/ahci: logical sector size > 512 is not supported");

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

	co_return true;
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

void Port::checkErrors() {
	auto is = regs_.load(regs::interruptStatus);

	// TODO: Make this more robust (try to recover)
	if (is & (flags::is::hostFatalError | flags::is::ifFatalError) || regs_.load(regs::tfd) & 0x1) {
		printf("\e[31mblock/ahci: Port %d encountered error\e[39m\n", portIndex_);
		dumpState();
		abort();
	} else if (is & flags::is::ifNonFatalError) {
		printf("\e[31mblock/ahci: Port %d encountered non-fatal error\e[39m\n", portIndex_);
		dumpState();
		abort();
	} else if (regs_.load(regs::tfd) & 1) {
		printf("\e[31mblock/ahci: Port %d encountered task file error\e[39m\n", portIndex_);
		dumpState();
		abort();
	}
}

void Port::handleIrq() {
	auto is = regs_.load(regs::interruptStatus);

	if (logCommands) {
		printf("block/ahci: Port %d handling IRQ: PxIS %#x, PxIE %#x, PxTFD %#x, PxCI %#x, PxCAS %#x\n",
				portIndex_, is, regs_.load(regs::interruptEnable), regs_.load(regs::tfd),
				regs_.load(regs::commandIssue), regs_.load(regs::commandAndStatus));
	}

	checkErrors();

	std::vector<Command *> completed;

	// Notify all completed commands
	auto cmdActiveMask = regs_.load(regs::commandIssue);
	for (size_t i = 0; i < numCommandSlots_; i++) {
		if (submittedCmds_[i] && !(cmdActiveMask & (1 << i))) {
			completed.push_back(std::exchange(submittedCmds_[i], nullptr));
		}
	}

	commandsInFlight_ -= completed.size();
	regs_.store(regs::interruptStatus, is);

	for (auto &cmd : completed) {
		cmd->notifyCompletion();
	}

	// If the buffer has gone from full to not full, wake the tasks waiting for a free slot.
	// TODO: If we have a lot of waiters, this will cause many spurious wakeups. Ideally, we only
	// notify a certain number of tasks, and the rest can stay asleep.
	if (commandsInFlight_ + completed.size() == numCommandSlots_ && completed.size() > 0) {
		freeSlotDoorbell_.raise();
	}
}

async::detached Port::submitPendingLoop_() {
	while (true) {
		auto cmd = co_await pendingCmdQueue_.async_get();
		assert(cmd);
		co_await submitCommand_(cmd.value());
	}
}

async::result<void> Port::submitCommand_(Command *cmd) {
	auto slot = co_await findFreeSlot_();
	assert(!(regs_.load(regs::commandIssue) & (1 << slot)));
	assert(!submittedCmds_[slot]);

	// Setup command table and FIS
	cmd->prepare(commandTables_[slot], commandList_->slots[slot]);

	// Issue command
	submittedCmds_[slot] = cmd;
	commandsInFlight_++;

	// Wait until not busy
	while (regs_.load(regs::tfd) & (flags::tfd::bsy | flags::tfd::drq))
		;

	regs_.store(regs::commandIssue, 1 << slot);
	co_return;
}

async::result<void> Port::readSectors(uint64_t sector, void *buffer, size_t numSectors) {
	Command cmd{sector, numSectors, numSectors * sectorSize,
			buffer, CommandType::read};
	pendingCmdQueue_.put(&cmd);
	co_await cmd.getFuture();
}

async::result<void> Port::writeSectors(uint64_t sector, const void *buffer, size_t numSectors) {
	Command cmd{sector, numSectors, numSectors * sectorSize,
			const_cast<void *>(buffer), CommandType::write};
	pendingCmdQueue_.put(&cmd);
	co_await cmd.getFuture();
}

async::result<size_t> Port::getSize() {
	assert(deviceSize_ != 0);
	co_return deviceSize_;
}
