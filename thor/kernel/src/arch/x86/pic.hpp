#ifndef THOR_ARCH_X86_PIC_HPP
#define THOR_ARCH_X86_PIC_HPP

#include "../../generic/irq.hpp"
#include "../../generic/types.hpp"
#include "../../generic/timer.hpp"

namespace thor {

// --------------------------------------------------------
// Local APIC management
// --------------------------------------------------------

struct GlobalApicContext {
	friend struct LocalApicContext;

	struct GlobalAlarmSlot : AlarmTracker {
		using AlarmTracker::fireAlarm;

		void arm(uint64_t nanos) override;
	};

	AlarmTracker *globalAlarm() {
		return &_globalAlarmInstance;
	}

private:
	GlobalAlarmSlot _globalAlarmInstance;

private:
	frigg::TicketLock _mutex;

	uint64_t _globalDeadline;
};

struct LocalApicContext {
	friend struct GlobalApicContext;

	static void setPreemption(uint64_t nanos);

	static void handleTimerIrq();

private:
	static void _updateLocalTimer();

private:
	uint64_t _preemptionDeadline;
};

GlobalApicContext *globalApicContext();

void initLocalApicOnTheSystem();
void initLocalApicPerCpu();

uint32_t getLocalApicId();

uint64_t localTicks();

void calibrateApicTimer();

void armPreemption(uint64_t nanos);
void disarmPreemption();

void acknowledgeIpi();

void raiseInitAssertIpi(uint32_t dest_apic_id);

void raiseInitDeassertIpi(uint32_t dest_apic_id);

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page);

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

void setupIoApic(PhysicalAddr address);

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void setupLegacyPic();

void maskLegacyPic();

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq);

IrqPin *getGlobalSystemIrq(size_t n);

} // namespace thor

#endif // THOR_ARCH_X86_PIC_HPP
