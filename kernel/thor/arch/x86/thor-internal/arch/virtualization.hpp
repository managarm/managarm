#pragma once

#include <hel.h>

#include <frg/optional.hpp>

#include <thor-internal/arch/vlapic.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor {

namespace x86_virt {

constexpr uint64_t XCR0_X87 = 1 << 0;
constexpr uint64_t XCR0_SSE = 1 << 1;
constexpr uint64_t XCR0_AVX = 1 << 2;
constexpr uint64_t XCR0_AVX512 = 7 << 5; // Opmask, ZMM_Hi256 and Hi16_ZMM.

constexpr uint8_t UD_VECTOR = 6;
constexpr uint8_t GP_VECTOR = 13;

// Types of events that we inject.
// VMX and SVM use the same encoding for these types.
constexpr uint8_t kEventExternalInterrupt = 0;
constexpr uint8_t kEventHardwareException = 3;
constexpr uint8_t kEventSoftwareInterrupt = 4;

bool isCanonical(uint64_t address);

// XSETBV raises #GP for all other values.
bool isValidXcr0(uint64_t value);

} // namespace x86_virt

struct X86VirtualizedCpu : VirtualizedCpu {
	virtual void storeBasicRegs(const HelX86VirtualizationBasicRegs *regs) = 0;
	virtual void loadBasicRegs(HelX86VirtualizationBasicRegs *regs) = 0;

	virtual bool storeMsrs(const HelX86VirtualizationMsrs *msrs) = 0;
	virtual void loadMsrs(HelX86VirtualizationMsrs *msrs) = 0;

	bool assertInterrupt(uint64_t number, bool level) override;

protected:
	// An event to inject on the next entry, an event whose delivery was interrupted by
	// a VM exit or an exception that an emulated instruction raised.
	struct Event {
		uint8_t type;
		uint8_t vector;
		bool hasErrorCode;
		uint32_t errorCode;
		// For software interrupts and exceptions.
		uint32_t instructionLength;
	};

	void queueException(uint8_t vector, bool hasErrorCode, uint32_t errorCode);

	// Validates the completion that is passed to run() and applies it.
	// Returns an error if run() must fail. If skipInstruction is set, the completed
	// instruction needs to be skipped (by advancing RIP by pendingCompletion_.instructionLength).
	frg::optional<Error> applyCompletion(const HelVmexitCompletion *completion,
			uint64_t &rax, uint64_t &rbx, uint64_t &rcx, uint64_t &rdx, bool &skipInstruction);

	// Records an exit that userspace needs to complete and return its HelVmexitReason.
	HelVmexitReason requestMsrCompletion(bool write, uint64_t rax, uint64_t rcx, uint64_t rdx,
			uint32_t instructionLength);
	HelVmexitReason requestCpuidCompletion(uint64_t rax, uint64_t rcx, uint32_t instructionLength);
	HelVmexitReason requestHyperCallCompletion(uint32_t instructionLength);
	HelVmexitReason requestIoCompletion(uint16_t port, uint8_t size, bool in, bool string,
			uint64_t rax, uint32_t instructionLength);

	// Returns false if the access raises #GP.
	bool emulateLapicMsr(uint32_t index, bool write, uint64_t now,
			VirtualLapic::WriteEffects &effects, uint64_t &rax, uint64_t &rdx);

	// Blocks until an interrupt can be delivered, the deadline passes, a kick is
	// pending or the thread is cancelled. Spurious wakeups are possible.
	void waitForGuestInterrupt(frg::optional<uint64_t> deadline, bool interruptsEnabled);

	bool reinjectValid_ = false;
	Event reinject_{};

	VirtualLapic lapic_;

	// The guest executed HLT and waits for an interrupt.
	// TODO: The VMM can neither read nor reset this state (e.g., to reset a halted vCPU).
	bool halted_ = false;

	// The exit that userspace needs to complete (see HelVmexitCompletion).
	struct PendingCompletion {
		uint32_t exitReason;
		// Length of the instruction that the completion skips.
		uint32_t instructionLength;
		// For port I/O.
		uint8_t ioSize;
		bool ioIn;
		bool ioString;
	};
	bool completionPending_ = false;
	PendingCompletion pendingCompletion_{};
};

} // namespace thor
