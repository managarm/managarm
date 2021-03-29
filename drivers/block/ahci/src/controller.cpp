#include <inttypes.h>

#include "controller.hpp"
#include "util.hpp"

namespace regs {
	constexpr arch::scalar_register<uint32_t> cap{0x0};
	constexpr arch::scalar_register<uint32_t> ghc{0x4};
	constexpr arch::scalar_register<uint32_t> interruptStatus{0x8};
	constexpr arch::scalar_register<uint32_t> portsImpl{0xC};
	constexpr arch::scalar_register<uint32_t> version{0x10};
	constexpr arch::scalar_register<uint32_t> cap2{0x24};
	constexpr arch::scalar_register<uint32_t> biosHandoff{0x28};
}

namespace flags {
	namespace ghc {
		constexpr int AHCI_ENABLE	   = 1 << 31;
		constexpr int INTERRUPT_ENABLE = 1 << 1;
		constexpr int HBA_RESET		   = 1;
	}

	namespace bohc {
		constexpr int BIOS_BUSY		 = 1 << 4;
		constexpr int OS_OWNERSHIP	 = 1 << 1;
		constexpr int BIOS_OWNERSHIP = 1;
	}

	namespace cap {
		constexpr int SUPPORTS_64BIT   = 1 << 31;
		constexpr int STAGGERED_SPINUP = 1 << 27;
	}

	namespace cap2 {
		constexpr int SUPPORTS_HANDOFF = 1;
	}
}

namespace {
	constexpr bool LOG_COMMANDS = false;
}

Controller::Controller(protocols::hw::Device hwDevice, helix::Mapping hbaRegs,
		helix::UniqueDescriptor ahciBar, helix::UniqueDescriptor irq)
	: hwDevice_{std::move(hwDevice)}, regsMapping_{std::move(hbaRegs)},
	regs_{regsMapping_.get()}, irq_{std::move(irq)} {
}

async::detached Controller::run() {
	// Enable AHCI
	auto ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::AHCI_ENABLE);

	// Perform BIOS -> OS handoff (10.6.3)
	auto version = regs_.load(regs::version);
	auto cap2 = regs_.load(regs::cap2);
	if (version >= 0x10200 && (cap2 & flags::cap2::SUPPORTS_HANDOFF)) {
		auto biosHandoff = regs_.load(regs::biosHandoff);
		regs_.store(regs::biosHandoff, biosHandoff | flags::bohc::OS_OWNERSHIP);

		// Spec is slightly unclear what to do here: first, wait on BOS = 0 for 25ms.
		auto timedOut = co_await kindaBusyWait(25'000'000,
				[&]{ return !(regs_.load(regs::biosHandoff) & flags::bohc::BIOS_OWNERSHIP); });

		if (timedOut) {
			// If BB is now set, we wait on BOS = 0 for 2 seconds.
			if (regs_.load(regs::biosHandoff) & flags::bohc::BIOS_BUSY) {
				std::cout << "block/ahci: BIOS handoff timed out once, retrying...\n";
				timedOut = co_await kindaBusyWait(2'000'000'000,
					[&]{ return !(regs_.load(regs::biosHandoff) & flags::bohc::BIOS_OWNERSHIP); });
				assert(!timedOut && "block/ahci: BIOS handoff timed out twice");
			} else {
				std::cout << "block/ahci: BIOS handoff timed out once, assuming control\n";
			}
		}
	}

	// Reset the controller (10.4.3)
	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::HBA_RESET);

	// Wait until the reset is complete (HR = 0), with a timeout of 1s
	auto timedOut = co_await kindaBusyWait(1'000'000'000,
		[&]{ return !(regs_.load(regs::ghc) & flags::ghc::HBA_RESET); });
	assert(!timedOut && "block/ahci: HBA timed out after reset");

	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::AHCI_ENABLE);

	auto cap = regs_.load(regs::cap);
	maxPorts_ = (cap & 0xF) + 1;
	assert(maxPorts_ <= 32);

	portsImpl_ = regs_.load(regs::portsImpl);
	assert(portsImpl_ != 0 && std::popcount(portsImpl_) <= maxPorts_);

	auto numCommandSlots = ((cap >> 8) & 0x1F) + 1;
	auto iss = (cap >> 20) & 0xF;
	bool ss = cap & flags::cap::STAGGERED_SPINUP;
	bool s64a = cap & flags::cap::SUPPORTS_64BIT;
	assert(s64a); // TODO: We aren't allowed to read some fields if no 64-bit support

	printf("block/ahci: Initialised controller: version %x, %d active ports, "
			"%d slots, Gen %d, SS %s, 64-bit %s\n", version, std::popcount(portsImpl_),
			numCommandSlots, iss, ss ? "yes" : "no", s64a ? "yes" : "no");

	if (!(co_await initPorts_(numCommandSlots))) {
		std::cout << "\e[31mblock/ahci: No ports found, exiting\e[39m\n";
		co_return;
	}

	// Enable interrupts
	co_await hwDevice_.enableBusIrq();
	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::INTERRUPT_ENABLE);

	handleIrqs_();

	for (auto& port : activePorts_) {
		port->run();
	}
}

async::detached Controller::handleIrqs_() {
	irqSequence_ = 0;

	while (true) {
		if (LOG_COMMANDS) {
			printf("block/ahci: Awaiting IRQ, seq %" PRIu64 ", status %x\n",
				irqSequence_, regs_.load(regs::interruptStatus));
		}

		auto await = co_await helix_ng::awaitEvent(irq_, irqSequence_);
		HEL_CHECK(await.error());
		irqSequence_ = await.sequence();

		if (LOG_COMMANDS) {
			printf("block/ahci: Received IRQ, seq %" PRIu64 ", status %x\n",
				irqSequence_, regs_.load(regs::interruptStatus));
		}

		auto intStatus = regs_.load(regs::interruptStatus) & portsImpl_;
		if (intStatus) {
			for (auto& port : activePorts_) {
				if (intStatus & (1 << port->getIndex())) {
					port->handleIrq();
				}
			}

			regs_.store(regs::interruptStatus, ~0);
			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, irqSequence_));
		} else {
			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckNack, irqSequence_));
		}
	}
}

async::result<bool> Controller::initPorts_(size_t numCommandSlots) {
	for (int i = 0; i < maxPorts_; i++) {
		if (portsImpl_ & (1 << i)) {
			auto offset = 0x100 + i * 0x80;
			auto port = std::make_unique<Port>(i, numCommandSlots, regs_.subspace(offset));

			if (co_await port->init())
				activePorts_.push_back(std::move(port));
		}
	}

	co_return activePorts_.size() > 0;
}
