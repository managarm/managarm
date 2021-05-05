#pragma once

#include <arch/mem_space.hpp>
#include <x86/machine.hpp>
#include <thor-internal/initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/types.hpp>

namespace thor {

// --------------------------------------------------------
// Local APIC management
// --------------------------------------------------------

struct ApicRegisterSpace {
	constexpr ApicRegisterSpace()
	: _x2apic{false}, _mem_base(0) { }

	ApicRegisterSpace(bool x2apic, void *base = nullptr)
	: _x2apic{x2apic}, _mem_base{reinterpret_cast<uintptr_t>(base)} { }

	template<typename RT>
	void store(RT r, typename RT::rep_type value) const {
		auto v = static_cast<typename RT::bits_type>(value);
		if(_x2apic) {
			auto msr = x2apic_msr_base + (r.offset() >> 4);
			common::x86::wrmsr(msr, v);
		} else {
			auto p = reinterpret_cast<typename RT::bits_type *>(_mem_base + r.offset());
			arch::mem_ops<typename RT::bits_type>::store(p, v);
		}
	}

	template<typename RT>
	typename RT::rep_type load(RT r) const {
		if(_x2apic) {
			auto msr = x2apic_msr_base + (r.offset() >> 4);
			return static_cast<typename RT::rep_type>(common::x86::rdmsr(msr));
		} else {
			auto p = reinterpret_cast<const typename RT::bits_type *>(_mem_base + r.offset());
			auto b = arch::mem_ops<typename RT::bits_type>::load(p);
			return static_cast<typename RT::rep_type>(b);
		}
	}

	bool isUsingX2apic() const {
		return _x2apic;
	}

private:
	bool _x2apic;
	uintptr_t _mem_base;
	static constexpr uint32_t x2apic_msr_base = 0x800;
};

struct GlobalApicContext {
	friend struct LocalApicContext;

	struct GlobalAlarmSlot final : AlarmTracker {
		using AlarmTracker::fireAlarm;

		void arm(uint64_t nanos) override;
	};

	AlarmTracker *globalAlarm() {
		return &_globalAlarmInstance;
	}

private:
	GlobalAlarmSlot _globalAlarmInstance;

private:
	frg::ticket_spinlock _mutex;

	uint64_t _globalDeadline;
};

struct LocalApicContext {
	friend struct GlobalApicContext;

	LocalApicContext();

	static void setPreemption(uint64_t nanos);

	static void handleTimerIrq();

private:
	static void _updateLocalTimer();

private:
	uint64_t _preemptionDeadline;
	uint64_t _globalDeadline;
};

GlobalApicContext *globalApicContext();

initgraph::Stage *getApicDiscoveryStage();

void initLocalApicPerCpu();

uint32_t getLocalApicId();

uint64_t localTicks();

void calibrateApicTimer();

void acknowledgeIpi();

void raiseInitAssertIpi(uint32_t dest_apic_id);

void raiseInitDeassertIpi(uint32_t dest_apic_id);

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page);

void sendShootdownIpi();
void sendGlobalNmi();

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

void setupIoApic(int apic_id, int gsi_base, PhysicalAddr address);

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void remapLegacyPic(int offset);
void maskLegacyPic();

bool checkLegacyPicIsr(int irq);

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq);

IrqPin *getGlobalSystemIrq(size_t n);

} // namespace thor
