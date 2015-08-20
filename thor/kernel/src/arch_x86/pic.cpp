
#include "../kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

uint32_t *localApicRegs;

enum {
	kLApicSpurious = 60,
	kLApicIcwLow = 192,
	kLApicIcwHigh = 196,
	kLApicLvtTimer = 200,
	kLApicInitialCount = 224
};

enum {
	kIcrDeliverInit = 0x500,
	kIcrDeliverStartup = 0x600,
	kIcrLevelAssert = 0x4000,
	kIcrTriggerLevel = 0x8000,
};

void initializeLocalApic() {
	uint64_t apic_info = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	ASSERT((apic_info & (1 << 11)) != 0); // local APIC is enabled
	localApicRegs = accessPhysical<uint32_t>(apic_info & 0xFFFFF000);
	
	// enable the local apic
	uint32_t spurious_vector = 0x81;
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicSpurious],
			spurious_vector | 0x100);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			kIcrDeliverInit | kIcrTriggerLevel | kIcrLevelAssert);
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			kIcrDeliverInit | kIcrTriggerLevel);
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	ASSERT((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::atomic::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			vector | kIcrDeliverStartup);
}

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void ioWait() { }

enum LegacyPicRegisters {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum LegacyPicFlags {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kPicEoi = 0x20
};

void remapLegacyPic(int offset) {
	// save masks
	uint8_t a1 = frigg::arch_x86::ioInByte(kPic1Data);
	uint8_t a2 = frigg::arch_x86::ioInByte(kPic2Data);

	// start initilization
	frigg::arch_x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic1Data, offset);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	frigg::arch_x86::ioOutByte(kPic1Data, 4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, 2);
	ioWait();

	frigg::arch_x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	frigg::arch_x86::ioOutByte(kPic1Data, a1);
	frigg::arch_x86::ioOutByte(kPic2Data, a2);
}

void setupLegacyPic() {
	remapLegacyPic(64);
}

void acknowledgeIrq(int irq) {
	if(irq >= 8)
		frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
	frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
}

} // namespace thor

