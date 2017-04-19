
#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/mem_space.hpp>
#include <arch/io_space.hpp>

#include "generic/fiber.hpp"
#include "generic/kernel.hpp"
#include "generic/irq.hpp"
#include "generic/service_helpers.hpp"

namespace thor {

arch::bit_register<uint32_t> lApicId(0x0020);
arch::scalar_register<uint32_t> lApicEoi(0x00B0);
arch::bit_register<uint32_t> lApicSpurious(0x00F0);
arch::bit_register<uint32_t> lApicIcrLow(0x0300);
arch::bit_register<uint32_t> lApicIcrHigh(0x0310);
arch::bit_register<uint32_t> lApicLvtTimer(0x0320);
arch::scalar_register<uint32_t> lApicInitCount(0x0380);
arch::scalar_register<uint32_t> lApicCurCount(0x0390);

// lApicId registers
arch::field<uint32_t, uint8_t> apicId(24, 8);

// lApicSpurious registers
arch::field<uint32_t, uint8_t> apicSpuriousVector(0, 8);
arch::field<uint32_t, bool> apicSpuriousSwEnable(8, 1);
arch::field<uint32_t, bool> apicSpuriousFocusProcessor(9, 1);
arch::field<uint32_t, bool> apicSpuriousEoiBroadcastSuppression(12, 1);

// lApicIcrLow registers
arch::field<uint32_t, uint8_t> apicIcrLowVector(0, 8);
arch::field<uint32_t, uint8_t> apicIcrLowDelivMode(8, 3);
arch::field<uint32_t, bool> apicIcrLowDestMode(11, 1);
arch::field<uint32_t, bool> apicIcrLowDelivStatus(12, 1);
arch::field<uint32_t, bool> apicIcrLowLevel(14, 1);
arch::field<uint32_t, bool> apicIcrLowTriggerMode(15, 1);
arch::field<uint32_t, uint8_t> apicIcrLowDestShortHand(18, 2);

// lApicIcrHigh registers
arch::field<uint32_t, uint8_t> apicIcrHighDestField(24, 8);

// lApicLvtTimer registers
arch::field<uint32_t, uint8_t> apicLvtVector(0, 8);

arch::mem_space picBase;

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

uint32_t apicTicksPerMilli;

void initLocalApicOnTheSystem() {
	uint64_t msr = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	assert(msr & (1 << 11)); // local APIC is enabled

	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	// TODO: Intel SDM specifies that we should mask out all
	// bits > the physical address limit of the msr.
	// For now we just assume that they are zero.
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), msr & ~PhysicalAddr{0xFFF},
			page_access::write);
	picBase = arch::mem_space(register_ptr);

	frigg::infoLogger() << "Booting on CPU #" << getLocalApicId() << frigg::endLog;
}

void initLocalApicPerCpu() {
	// enable the local apic
	uint32_t spurious_vector = 0x81;
	picBase.store(lApicSpurious, apicSpuriousVector(spurious_vector)
			| apicSpuriousSwEnable(true));
	
	// setup a timer interrupt for scheduling
	uint32_t schedule_vector = 0x82;
	picBase.store(lApicLvtTimer, apicLvtVector(schedule_vector));
}

uint32_t getLocalApicId() {
	return picBase.load(lApicId) & apicId;
}

uint64_t localTicks() {
	return picBase.load(lApicCurCount);
}

void calibrateApicTimer() {
	const uint64_t millis = 100;
	picBase.store(lApicInitCount, 0xFFFFFFFF);
	pollSleepNano(millis * 1000000);
	uint32_t elapsed = 0xFFFFFFFF
			- picBase.load(lApicCurCount);
	picBase.store(lApicInitCount, 0);
	apicTicksPerMilli = elapsed / millis;
	
	frigg::infoLogger() << "Local elapsed ticks: " << elapsed << frigg::endLog;
}

void preemptThisCpu(uint64_t slice_nano) {
	assert(apicTicksPerMilli > 0);
	
	uint64_t ticks = (slice_nano / 1000000) * apicTicksPerMilli;
	if(ticks == 0)
		ticks = 1;
	picBase.store(lApicInitCount, ticks);
}

void acknowledgePreemption() {
	picBase.store(lApicEoi, 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, Level:assert = 1, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowLevel(true) | apicIcrLowTriggerMode(true));
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowTriggerMode(true));
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:startup = 6
	picBase.store(lApicIcrLow, apicIcrLowVector(vector)
			| apicIcrLowDelivMode(6));
}

// --------------------------------------------------------

IrqPin *globalSystemIrqs[24];

IrqPin *getGlobalSystemIrq(size_t n) {
	assert(n <= 24);
	return globalSystemIrqs[n];
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

// TODO: Replace this by proper IRQ allocation.
extern frigg::LazyInitializer<IrqSlot> globalIrqSlots[24];

constexpr arch::scalar_register<uint32_t> apicIndex(0x00);
constexpr arch::scalar_register<uint32_t> apicData(0x10);

namespace pin_word1 {
	constexpr arch::field<uint32_t, unsigned int> vector(0, 8);
	constexpr arch::field<uint32_t, unsigned int> deliveryMode(8, 3);
	constexpr arch::field<uint32_t, bool> logicalMode(11, 1);
	constexpr arch::field<uint32_t, bool> deliveryStatus(12, 1);
	constexpr arch::field<uint32_t, bool> activeLow(13, 1);
	constexpr arch::field<uint32_t, bool> remotePending(14, 1);
	constexpr arch::field<uint32_t, bool> levelTriggered(15, 1);
	constexpr arch::field<uint32_t, bool> masked(16, 1);
};

namespace pin_word2 {
	constexpr arch::field<uint32_t, unsigned int> destination(24, 8);
};

namespace {
	enum {
		kIoApicId = 0,
		kIoApicVersion = 1,
		kIoApicInts = 16,
	};

	struct IoApic {
	public:
		struct Pin : IrqPin {
			Pin(IoApic *chip, unsigned int index);

			IrqStrategy program(TriggerMode mode, Polarity polarity) override;
			void mask() override;
			void unmask() override;
			void sendEoi() override;

		private:
			IoApic *_chip;
			unsigned int _index;
			
			// The following variables store the current pin configuration.
			bool _levelTriggered;
			bool _activeLow;
		};

		IoApic(arch::mem_space space);

		size_t pinCount();

		IrqPin *accessPin(size_t n);

	private:
		uint32_t _loadRegister(uint32_t index) {
			_space.store(apicIndex, index);
			return _space.load(apicData);
		}

		void _storeRegister(uint32_t index, uint32_t value) {
			_space.store(apicIndex, index);
			_space.store(apicData, value);
		}

		arch::mem_space _space;
		size_t _numPins;
		// TODO: Replace by dyn_array?
		Pin **_pins;
	};

	static frigg::String<KernelAlloc> buildName(unsigned int index) {
		return frigg::String<KernelAlloc>{*kernelAlloc, "io-apic."}
				+ frigg::to_string(*kernelAlloc, index);
	}

	IoApic::Pin::Pin(IoApic *chip, unsigned int index)
	: IrqPin{buildName(index)}, _chip{chip}, _index{index} { }

	IrqStrategy IoApic::Pin::program(TriggerMode mode, Polarity polarity) {
		IrqStrategy strategy;
		if(mode == TriggerMode::edge) {
			_levelTriggered = false;
			strategy = IrqStrategy::justEoi;
		}else{
			assert(mode == TriggerMode::level);
			_levelTriggered = true;
			strategy = IrqStrategy::maskThenEoi;
		}

		if(polarity == Polarity::high) {
			_activeLow = false;
		}else{
			assert(polarity == Polarity::low);
			_activeLow = true;
		}

		auto vector = 64 + _index;
		_chip->_storeRegister(kIoApicInts + _index * 2 + 1,
				static_cast<uint32_t>(pin_word2::destination(0)));
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
		return strategy;
	}
	
	void IoApic::Pin::mask() {
//		frigg::infoLogger() << "thor: Masking pin " << _index << frigg::endLog;
		auto vector = 64 + _index;
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow) | pin_word1::masked(true)));
	}

	void IoApic::Pin::unmask() {
//		frigg::infoLogger() << "thor: Unmasking pin " << _index << frigg::endLog;
		auto vector = 64 + _index;
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
	}

	void IoApic::Pin::sendEoi() {
		acknowledgeIrq(0);
	}

	IoApic::IoApic(arch::mem_space space)
	: _space{std::move(space)} {
		_numPins = ((_loadRegister(kIoApicVersion) >> 16) & 0xFF) + 1;
		frigg::infoLogger() << "thor: I/O APIC supports "
				<< _numPins << " pins" << frigg::endLog;

		_pins = frigg::constructN<Pin *>(*kernelAlloc, _numPins);
		for(size_t i = 0; i < _numPins; i++) {
			_pins[i] = frigg::construct<Pin>(*kernelAlloc, this, i);

			// Dump interesting configurations.
			arch::bit_value<uint32_t> current{_loadRegister(kIoApicInts + i * 2)};
			if(!(current & pin_word1::masked))
				frigg::infoLogger() << "    Pin " << i << " was not masked by BIOS."
						<< frigg::endLog;

			// Mask all interrupts before they are configured.
			_storeRegister(kIoApicInts + i * 2, static_cast<uint32_t>(pin_word1::masked(true)));
		}
	}

	size_t IoApic::pinCount() {
		return _numPins;
	}

	IrqPin *IoApic::accessPin(size_t n) {
		return _pins[n];
	}
}

void setupIoApic(PhysicalAddr address) {
	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write);
	
	picModel = kModelApic;

	auto apic = frigg::construct<IoApic>(*kernelAlloc, arch::mem_space{register_ptr});
	for(size_t i = 0; i < frigg::min(apic->pinCount(), size_t{24}); i++) {
		auto pin = apic->accessPin(i);
		globalSystemIrqs[i] = pin;
		globalIrqSlots[i]->link(pin);
	}

	KernelFiber::run([=] {
		while(true) {
			for(size_t i = 0; i < apic->pinCount(); ++i)
				apic->accessPin(i)->warnIfPending();

			fiberSleep(500000000);
		}
	});
}

/*

uint32_t *ioApicRegs;
arch::mem_space ioApicBase;

void setupIoApic(PhysicalAddr address) {
	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write);
	ioApicBase = arch::mem_space(register_ptr);
	
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
*/

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
	remapLegacyPic(32);
}

void maskLegacyPic() {
	frigg::arch_x86::ioOutByte(kPic1Data, 0xFF);
	frigg::arch_x86::ioOutByte(kPic2Data, 0xFF);
}

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

// TODO: Split this function in two: One for the legacy PIC and one for APIC.
void acknowledgeIrq(int irq) {
	if(picModel == kModelApic) {
		picBase.store(lApicEoi, 0);
	}else if(picModel == kModelLegacy) {
		if(irq >= 8)
			frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
		frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor

