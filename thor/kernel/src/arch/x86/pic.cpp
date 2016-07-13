
#include "generic/kernel.hpp"

namespace thor {

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

uint32_t *localApicRegs;
uint32_t apicTicksPerMilli;

enum {
	kLApicId = 8,
	kLApicEoi = 44,
	kLApicSpurious = 60,
	kLApicIcwLow = 192,
	kLApicIcwHigh = 196,
	kLApicLvtTimer = 200,
	kLApicInitialCount = 224,
	kLApicCurrentCount = 228
};

enum {
	kIcrDeliverInit = 0x500,
	kIcrDeliverStartup = 0x600,
	kIcrLevelAssert = 0x4000,
	kIcrTriggerLevel = 0x8000
};

void initLocalApicOnTheSystem() {
	uint64_t apic_info = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	assert((apic_info & (1 << 11)) != 0); // local APIC is enabled
	localApicRegs = accessPhysical<uint32_t>(apic_info & 0xFFFFF000);

	frigg::infoLogger() << "Booting on CPU #" << getLocalApicId() << frigg::endLog;
}

void initLocalApicPerCpu() {
	// enable the local apic
	uint32_t spurious_vector = 0x81;
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicSpurious], spurious_vector | 0x100);
	
	// setup a timer interrupt for scheduling
	uint32_t schedule_vector = 0x82;
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicLvtTimer], schedule_vector);
}

uint32_t getLocalApicId() {
	return (frigg::volatileRead<uint32_t>(&localApicRegs[kLApicId]) >> 24) & 0xFF;
}

void calibrateApicTimer() {
	const uint64_t millis = 100;
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicInitialCount], 0xFFFFFFFF);
	pollSleepNano(millis * 1000000);
	uint32_t elapsed = 0xFFFFFFFF
			- frigg::volatileRead<uint32_t>(&localApicRegs[kLApicCurrentCount]);
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicInitialCount], 0);
	apicTicksPerMilli = elapsed / millis;
	
	frigg::infoLogger() << "Local elapsed ticks: " << elapsed << frigg::endLog;
}

void preemptThisCpu(uint64_t slice_nano) {
	assert(apicTicksPerMilli > 0);
	
	uint64_t ticks = (slice_nano / 1000000) * apicTicksPerMilli;
	if(ticks == 0)
		ticks = 1;
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicInitialCount], ticks);
}

void acknowledgePreemption() {
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicEoi], 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			kIcrDeliverInit | kIcrTriggerLevel | kIcrLevelAssert);
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			kIcrDeliverInit | kIcrTriggerLevel);
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwHigh],
			dest_apic_id << 24);
	frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicIcwLow],
			vector | kIcrDeliverStartup);
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

enum {
	kIoApicId = 0,
	kIoApicVersion = 1,
	kIoApicInts = 16,
};

uint32_t *ioApicRegs;

uint32_t readIoApic(uint32_t index) {
	frigg::volatileWrite<uint32_t>(&ioApicRegs[0], index);
	return frigg::volatileRead<uint32_t>(&ioApicRegs[4]);
}
void writeIoApic(uint32_t index, uint32_t value) {
	frigg::volatileWrite<uint32_t>(&ioApicRegs[0], index);
	frigg::volatileWrite<uint32_t>(&ioApicRegs[4], value);
}

void setupIoApic(PhysicalAddr address) {
	ioApicRegs = accessPhysical<uint32_t>(address);
	
	picModel = kModelApic;
	maskLegacyPic();

	int num_ints = ((readIoApic(kIoApicVersion) >> 16) & 0xFF) + 1;
	frigg::infoLogger() << "I/O APIC supports " << num_ints << " interrupts" << frigg::endLog;

	for(int i = 0; i < num_ints; i++) {
		uint32_t vector = 64 + i;
		writeIoApic(kIoApicInts + i * 2, vector);
		writeIoApic(kIoApicInts + i * 2 + 1, 0);
	}
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

void maskLegacyPic() {
	frigg::arch_x86::ioOutByte(kPic1Data, 0xFF);
	frigg::arch_x86::ioOutByte(kPic2Data, 0xFF);
}

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq) {
	if(picModel == kModelApic) {
		frigg::volatileWrite<uint32_t>(&localApicRegs[kLApicEoi], 0);
	}else if(picModel == kModelLegacy) {
		if(irq >= 8)
			frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
		frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor

