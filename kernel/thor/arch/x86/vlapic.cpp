#include <string.h>

#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch/vlapic.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

namespace {
	constexpr uint32_t apicVersion = 0x14;
	// The index of the last LVT entry (error); there is no CMCI entry.
	constexpr uint32_t maxLvtEntry = 5;

	// The default base, BSP, x2APIC mode and enabled.
	constexpr uint64_t fixedApicBase = 0xFEE0'0000 | (1 << 8) | (1 << 10) | (1 << 11);
	// In x2APIC mode, the LDR is derived from the ID (cluster 0, bit 0 for ID 0).
	constexpr uint32_t fixedLdr = 1;

	constexpr uint32_t svrEnable = 1 << 8;
	constexpr uint32_t lvtMask = 1 << 16;
	constexpr uint32_t icrDeliveryStatus = 1 << 12;
	constexpr uint32_t icrLogical = 1 << 11;

	enum : unsigned {
		lvtTimer = 0,
		lvtThermal,
		lvtPerf,
		lvtLint0,
		lvtLint1,
		lvtError,
		numLvts,
	};

	// The writable bits of the LVT entries.
	constexpr uint32_t lvtWritable[numLvts] = {
		0x700FF, // Timer: vector, mask, mode.
		0x107FF, // Thermal: vector, delivery mode, mask.
		0x107FF, // Performance counter: vector, delivery mode, mask.
		0x1A7FF, // LINT0: vector, delivery mode, polarity, trigger mode, mask.
		0x1A7FF, // LINT1.
		0x100FF, // Error: vector, mask.
	};

	enum : unsigned {
		timerOneShot = 0,
		timerPeriodic = 1,
		timerTscDeadline = 2,
	};

	// Register offsets in the xAPIC page (x2APIC MSR = 0x800 + offset / 16).
	enum : uint32_t {
		regId = 0x020,
		regVersion = 0x030,
		regTpr = 0x080,
		regPpr = 0x0A0,
		regEoi = 0x0B0,
		regLdr = 0x0D0,
		regSvr = 0x0F0,
		regIsr = 0x100,
		regTmr = 0x180,
		regIrr = 0x200,
		regEsr = 0x280,
		regIcr = 0x300,
		// The upper half of the ICR (which is a single 64-bit MSR in x2APIC mode).
		regIcrHigh = 0x310,
		regLvt = 0x320,
		regInitialCount = 0x380,
		regCurrentCount = 0x390,
		regDivide = 0x3E0,
		regSelfIpi = 0x3F0,
	};

	// The 256-bit registers (ISR, TMR, IRR) occupy eight 16-byte slots each.
	bool inArray(uint32_t offset, uint32_t base) {
		return offset >= base && offset < base + 8 * 0x10;
	}

	bool inLvt(uint32_t offset) {
		return offset >= regLvt && offset < regLvt + numLvts * 0x10;
	}
}

VirtualLapic::VirtualLapic() {
	page_ = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
	PageAccessor accessor{page_};
	memset(accessor.get(), 0, kPageSize);
	// The physical memory stays mapped after the accessor is gone.
	regs_ = reinterpret_cast<uint32_t *>(accessor.get());

	reg_(regVersion) = apicVersion | (maxLvtEntry << 16);
	reg_(regLdr) = fixedLdr;
	reg_(regSvr) = 0xFF;
	for(unsigned i = 0; i < numLvts; i++)
		reg_(regLvt + i * 0x10) = lvtMask;
}

VirtualLapic::~VirtualLapic() {
	physicalAllocator->free(page_, kPageSize);
}

uint64_t VirtualLapic::apicBase() {
	return fixedApicBase;
}

uint32_t &VirtualLapic::reg_(uint32_t offset) {
	return regs_[offset / 4];
}

int VirtualLapic::highestBit_(uint32_t base) {
	for(int i = 7; i >= 0; i--) {
		if(auto word = reg_(base + i * 0x10); word)
			return i * 32 + (31 - __builtin_clz(word));
	}
	return -1;
}

void VirtualLapic::setBit_(uint32_t base, uint8_t bit) {
	reg_(base + (bit / 32) * 0x10) |= UINT32_C(1) << (bit % 32);
}

void VirtualLapic::clearBit_(uint32_t base, uint8_t bit) {
	reg_(base + (bit / 32) * 0x10) &= ~(UINT32_C(1) << (bit % 32));
}

bool VirtualLapic::testBit_(uint32_t base, uint8_t bit) {
	return reg_(base + (bit / 32) * 0x10) & (UINT32_C(1) << (bit % 32));
}

uint8_t VirtualLapic::ppr_() {
	uint8_t tpr = reg_(regTpr);
	int isrv = highestBit_(regIsr);
	uint8_t isrClass = isrv < 0 ? 0 : (isrv & 0xF0);
	if((tpr & 0xF0) >= isrClass)
		return tpr;
	return isrClass;
}

unsigned VirtualLapic::timerMode_() {
	return (reg_(regLvt + lvtTimer * 0x10) >> 17) & 3;
}

uint64_t VirtualLapic::timerDivider_() {
	// Bits 0, 1 and 3 encode the divider; 0b111 means 1.
	uint32_t config = reg_(regDivide);
	unsigned value = ((config >> 1) & 4) | (config & 3);
	if(value == 7)
		return 1;
	return UINT64_C(2) << value;
}

void VirtualLapic::acceptPosted_() {
	for(unsigned i = 0; i < 8; i++) {
		if(!posted_[i].load())
			continue;
		uint32_t bits = posted_[i].exchange(0);
		uint32_t level = postedLevel_[i].load();
		// A software-disabled local APIC does not accept fixed interrupts.
		if(!(reg_(regSvr) & svrEnable))
			continue;
		reg_(regIrr + i * 0x10) |= bits;
		auto &tmr = reg_(regTmr + i * 0x10);
		tmr = (tmr & ~bits) | (level & bits);
	}
}

void VirtualLapic::raise_(uint8_t vector) {
	// A software-disabled local APIC does not accept fixed interrupts.
	if(!(reg_(regSvr) & svrEnable))
		return;
	setBit_(regIrr, vector);
	clearBit_(regTmr, vector);
}

void VirtualLapic::startTimer_(uint64_t now) {
	unsigned mode = timerMode_();
	if(mode == timerTscDeadline)
		return;
	uint32_t initialCount = reg_(regInitialCount);
	if(!initialCount) {
		deadline_ = 0;
		period_ = 0;
		return;
	}
	uint64_t nanos = initialCount * timerDivider_();
	deadline_ = now + nanos;
	period_ = mode == timerPeriodic ? nanos : 0;
}

void VirtualLapic::sendIpi_() {
	uint32_t low = reg_(regIcr);
	uint32_t destination = reg_(regIcrHigh);
	uint8_t vector = low & 0xFF;
	unsigned mode = (low >> 8) & 7;
	unsigned shorthand = (low >> 18) & 3;

	bool self;
	switch(shorthand) {
	case 0:
		if(destination == UINT32_MAX)
			self = true;
		else if(low & icrLogical)
			self = !(destination >> 16) && (destination & fixedLdr);
		else
			self = !destination;
		break;
	case 1: // Self.
	case 2: // All including self.
		self = true;
		break;
	default: // All excluding self.
		self = false;
		break;
	}

	// Only fixed and lowest priority IPIs are supported; their trigger mode is ignored.
	if(self && mode <= 1 && vector >= 16)
		raise_(vector);
}

bool VirtualLapic::post(uint8_t vector, bool level) {
	if(vector < 16)
		return false;
	uint32_t bit = UINT32_C(1) << (vector % 32);
	// The trigger mode needs to be in place when acceptPosted_() sees the interrupt.
	if(level)
		postedLevel_[vector / 32].fetch_or(bit);
	else
		postedLevel_[vector / 32].fetch_and(~bit);
	posted_[vector / 32].fetch_or(bit);
	return true;
}

int VirtualLapic::tprThreshold() {
	acceptPosted_();
	int vector = highestBit_(regIrr);
	if(vector < 0)
		return -1;
	int isrv = highestBit_(regIsr);
	if(isrv >= 0 && (vector & 0xF0) <= (isrv & 0xF0))
		return -1;
	return vector >> 4;
}

int VirtualLapic::deliverableVector() {
	acceptPosted_();
	int vector = highestBit_(regIrr);
	if(vector < 0 || (vector & 0xF0) <= (ppr_() & 0xF0))
		return -1;
	return vector;
}

void VirtualLapic::acknowledge(uint8_t vector) {
	clearBit_(regIrr, vector);
	setBit_(regIsr, vector);
}

void VirtualLapic::checkTimer(uint64_t now) {
	if(!deadline_ || now < deadline_)
		return;
	uint32_t lvt = reg_(regLvt + lvtTimer * 0x10);
	uint8_t vector = lvt & 0xFF;
	if(!(lvt & lvtMask) && vector >= 16)
		raise_(vector);
	if(period_) {
		deadline_ += period_;
		// Do not try to catch up if we missed periods.
		if(deadline_ <= now)
			deadline_ = now + period_;
	}else{
		deadline_ = 0;
		tscDeadline_ = 0;
	}
}

frg::optional<uint64_t> VirtualLapic::timerDeadline() {
	if(!deadline_)
		return frg::null_opt;
	return deadline_;
}

uint64_t VirtualLapic::cr8() {
	return (reg_(regTpr) >> 4) & 0xF;
}

void VirtualLapic::setCr8(uint64_t value) {
	reg_(regTpr) = (value & 0xF) << 4;
}

bool VirtualLapic::isStoredRegister_(uint32_t offset) {
	switch(offset) {
	case regId:
	case regVersion:
	case regTpr:
	case regLdr:
	case regSvr:
	case regEsr: // We do not report errors.
	case regInitialCount:
	case regDivide:
		return true;
	default:
		break;
	}
	return inArray(offset, regIsr) || inArray(offset, regTmr) || inArray(offset, regIrr)
			|| inLvt(offset);
}

bool VirtualLapic::readRegister_(uint32_t offset, uint64_t now, uint64_t &value) {
	value = 0;
	switch(offset) {
	case regPpr: value = ppr_(); return true;
	case regIcr: value = reg_(regIcr) | (uint64_t{reg_(regIcrHigh)} << 32); return true;
	case regCurrentCount: {
		unsigned mode = timerMode_();
		if((mode == timerOneShot || mode == timerPeriodic) && deadline_ && now < deadline_) {
			uint64_t remaining = (deadline_ - now) / timerDivider_();
			value = remaining > UINT32_MAX ? UINT32_MAX : remaining;
		}
		return true;
	}
	default:
		break;
	}
	if(isStoredRegister_(offset)) {
		value = reg_(offset);
		return true;
	}
	// Write-only (EOI, SELF_IPI), reserved or unsupported (e.g., CMCI).
	return false;
}

bool VirtualLapic::writeRegister_(uint32_t offset, uint64_t value, uint64_t now,
		WriteEffects &effects) {
	switch(offset) {
	case regTpr:
		if(value > 0xFF)
			return false;
		reg_(regTpr) = value;
		return true;
	case regEoi: {
		if(value)
			return false;
		int vector = highestBit_(regIsr);
		if(vector >= 0) {
			clearBit_(regIsr, vector);
			if(testBit_(regTmr, vector)) {
				clearBit_(regTmr, vector);
				effects.levelEoi = vector;
			}
		}
		return true;
	}
	case regSvr:
		if(value & ~UINT64_C(0x1FF))
			return false;
		reg_(regSvr) = value;
		// Software-disabling the local APIC masks the LVT entries.
		if(!(value & svrEnable)) {
			for(unsigned i = 0; i < numLvts; i++)
				reg_(regLvt + i * 0x10) |= lvtMask;
		}
		return true;
	case regEsr:
		return !value;
	case regIcr:
		reg_(regIcr) = static_cast<uint32_t>(value) & ~icrDeliveryStatus;
		reg_(regIcrHigh) = value >> 32;
		sendIpi_();
		return true;
	case regInitialCount:
		reg_(regInitialCount) = value;
		startTimer_(now);
		return true;
	case regDivide:
		if(value & ~UINT64_C(0xB))
			return false;
		reg_(regDivide) = value;
		return true;
	case regSelfIpi:
		if(value > 0xFF)
			return false;
		if(value >= 16)
			raise_(value);
		return true;
	default:
		break;
	}
	if(inLvt(offset)) {
		unsigned entry = (offset - regLvt) / 0x10;
		uint32_t newValue = value & lvtWritable[entry];
		if(!(reg_(regSvr) & svrEnable))
			newValue |= lvtMask;
		if(entry == lvtTimer && ((newValue >> 17) & 3) != timerMode_()) {
			// Changing the timer mode disarms the timer.
			deadline_ = 0;
			period_ = 0;
			tscDeadline_ = 0;
		}
		reg_(offset) = newValue;
		return true;
	}
	// Read-only, reserved or unsupported.
	return false;
}

bool VirtualLapic::readMsr(uint32_t index, uint64_t now, uint64_t &value) {
	if(index == kMsrApicBase) {
		value = fixedApicBase;
		return true;
	}
	if(index == kMsrTscDeadline) {
		value = timerMode_() == timerTscDeadline ? tscDeadline_ : 0;
		return true;
	}
	return readRegister_((index - 0x800) << 4, now, value);
}

bool VirtualLapic::writeMsr(uint32_t index, uint64_t value, uint64_t now, WriteEffects &effects) {
	// The local APIC cannot be disabled or switched to xAPIC mode.
	if(index == kMsrApicBase)
		return value == fixedApicBase;
	if(index == kMsrTscDeadline) {
		// Writes in other modes are ignored.
		if(timerMode_() == timerTscDeadline) {
			tscDeadline_ = value;
			period_ = 0;
			if(!value) {
				deadline_ = 0;
			}else{
				// The guest's TSC equals the host's. Reading the clock after the TSC and adding 2 ns
				// for the round-down of both conversions keeps the timer from firing early.
				uint64_t tsc = getRawTimestampCounter();
				uint64_t nanos = value > tsc ? tscTicksToNanos(value - tsc) : 0;
				uint64_t current = getClockNanos();
				deadline_ = nanos >= UINT64_MAX - current - 1 ? UINT64_MAX : current + nanos + 2;
			}
		}
		return true;
	}
	// All registers except the ICR have 32 bits; writes to the upper half raise #GP.
	if(index != 0x830 && (value >> 32))
		return false;
	return writeRegister_((index - 0x800) << 4, value, now, effects);
}

} // namespace thor
