#pragma once

#include <atomic>
#include <stdint.h>

#include <frg/optional.hpp>
#include <thor-internal/types.hpp>

namespace thor {

constexpr uint32_t kMsrApicBase = 0x1B;
constexpr uint32_t kMsrTscDeadline = 0x6E0;

// Whether the virtual local APIC handles an MSR (the x2APIC MSRs, IA32_APIC_BASE and
// IA32_TSC_DEADLINE).
inline bool isVirtualLapicMsr(uint32_t index) {
	return index == kMsrApicBase || index == kMsrTscDeadline
			|| (index >= 0x800 && index <= 0x8FF);
}

// Models the registers of a vCPU's x2APIC (see the vCPU calls in hel.h); the vCPU delivers
// its interrupts. post() is thread-safe; everything else requires VirtualizedCpu::UseGuard.
struct VirtualLapic {
	// Side effects of a register write that the vCPU carries out.
	struct WriteEffects {
		// The vector of a level-triggered interrupt that the guest ended (EOI), or -1.
		int levelEoi = -1;
	};

	VirtualLapic();
	VirtualLapic(const VirtualLapic &) = delete;
	~VirtualLapic();

	VirtualLapic &operator=(const VirtualLapic &) = delete;

	// The MSR accesses of the guest. They return false if the access raises #GP.
	// now is the current time of the monotonic clock.
	bool readMsr(uint32_t index, uint64_t now, uint64_t &value);
	bool writeMsr(uint32_t index, uint64_t value, uint64_t now, WriteEffects &effects);

	// The value of IA32_APIC_BASE (which the guest cannot change).
	static uint64_t apicBase();

	// Queues a fixed-mode interrupt; the next deliverableVector() accepts it (a
	// software-disabled local APIC drops it), so the caller must notify() the vCPU.
	// Returns false for vectors below 16.
	bool post(uint8_t vector, bool level);

	// Accepts posted interrupts, then returns the highest pending vector whose priority is
	// above the PPR, or -1.
	int deliverableVector();

	// Moves the vector from the IRR to the ISR once it is injected.
	void acknowledge(uint8_t vector);

	// Raises the timer interrupt if the timer expired at time now.
	void checkTimer(uint64_t now);

	// The time at which the timer expires next (null if the timer is not armed).
	frg::optional<uint64_t> timerDeadline();

	// CR8 is an alias of TPR[7:4].
	uint64_t cr8();
	void setCr8(uint64_t value);

private:
	// Registers by their offset in the xAPIC page.
	uint32_t &reg_(uint32_t offset);
	// The 256-bit registers (ISR, TMR, IRR) by the offset of their first word.
	int highestBit_(uint32_t base);
	void setBit_(uint32_t base, uint8_t bit);
	void clearBit_(uint32_t base, uint8_t bit);
	bool testBit_(uint32_t base, uint8_t bit);

	uint8_t ppr_();
	unsigned timerMode_();
	uint64_t timerDivider_();
	void acceptPosted_();
	// Accepts an edge-triggered interrupt.
	void raise_(uint8_t vector);
	void startTimer_(uint64_t now);
	void sendIpi_();
	// Whether reads of the register return the value in the page.
	static bool isStoredRegister_(uint32_t offset);
	// They return false if the access raises #GP.
	bool readRegister_(uint32_t offset, uint64_t now, uint64_t &value);
	bool writeRegister_(uint32_t offset, uint64_t value, uint64_t now, WriteEffects &effects);

	// The registers live in a page with the layout of the xAPIC's MMIO page so that APICv
	// (Intel) and AVIC (AMD), which operate on such a page, can use it later.
	PhysicalAddr page_;
	uint32_t *regs_;

	// Interrupts that were posted but not accepted yet, and their trigger modes.
	std::atomic<uint32_t> posted_[8]{};
	std::atomic<uint32_t> postedLevel_[8]{};

	// The value of IA32_TSC_DEADLINE (TSC-deadline mode only).
	uint64_t tscDeadline_ = 0;
	// The time at which the timer expires (0: not armed) and its period (periodic mode).
	uint64_t deadline_ = 0;
	uint64_t period_ = 0;
};

} // namespace thor
