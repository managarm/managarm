#include <assert.h>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

constinit ExternalIrqController externalIrq{};

extern "C" void *thorExcVectors;

ClaimedExternalIrq claimGicV2Irq() {
	assert(std::holds_alternative<GicV2 *>(externalIrq));
	auto *gic = std::get<GicV2 *>(externalIrq);
	auto [cpu, irq] = gic->getIrq();
	return {cpu, irq, gic->getPin(irq)};
}

ClaimedExternalIrq claimGicV3Irq() {
	assert(std::holds_alternative<GicV3 *>(externalIrq));
	auto *gic = std::get<GicV3 *>(externalIrq);
	auto [cpu, irq] = gic->getIrq();
	return {cpu, irq, gic->getPin(irq)};
}

initgraph::Stage *getIrqControllerReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.irq-controller-ready"};
	return &s;
}

void initializeIrqVectors() { asm volatile("msr vbar_el1, %0" ::"r"(&thorExcVectors)); }

extern "C" void onPlatformInvalidException(FaultImageAccessor image) {
	thor::panicLogger() << "thor: an invalid exception has occured" << frg::endlog;
}

void handleGicIrq(IrqImageAccessor image, ClaimedExternalIrq irq);

namespace {

Word mmuAbortError(uint64_t esr) {
	Word errorCode = 0;

	auto ec = esr >> 26;
	auto iss = esr & ((1 << 25) - 1);

	// Originated from EL0
	if (ec == 0x20 || ec == 0x24)
		errorCode |= kPfUser;

	// Is an instruction abort
	if (ec == 0x20 || ec == 0x21) {
		errorCode |= kPfInstruction;
	} else {
		if (iss & (1 << 6))
			errorCode |= kPfWrite;
	}

	auto sc = iss & 0x3F;

	if (sc < 16) {
		auto type = (sc >> 2) & 0b11;
		if (type == 0) // Address size fault
			errorCode |= kPfBadTable;
		if (type != 1) // Not a translation fault
			errorCode |= kPfAccess;
	}

	return errorCode;
}

bool updatePageAccess(FaultImageAccessor image, Word error) {
	if ((error & kPfWrite) && (error & kPfAccess) && !inHigherHalf(*image.faultAddr())) {
		// Check if it's just a writable page that's not dirty yet
		smarter::borrowed_ptr<Thread> this_thread = getCurrentThread();
		// TODO: We pass flags = 0 for now since updatePageAccess() on aarch64
		//       currently does not use the flags.
		return this_thread->getAddressSpace()->updatePageAccess(
		    *image.faultAddr() & ~(kPageSize - 1), 0
		);
	}

	return false;
}


} // namespace

void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);
void handleOtherFault(FaultImageAccessor image, Interrupt fault);
void handleSyscall(SyscallImageAccessor image);

constexpr bool logUpdatePageAccess = false;

extern "C" void onPlatformSyncFault(FaultImageAccessor image) {
	auto ec = *image.code() >> 26;

	enableInts();

	switch (ec) {
		case 0x00: // Invalid
		case 0x18: // Trapped MSR, MRS, or System instruction
			handleOtherFault(image, kIntrIllegalInstruction);
			break;
		case 0x20:   // Instruction abort, lower EL
		case 0x21:   // Instruction abort, same EL
		case 0x24:   // Data abort, lower EL
		case 0x25: { // Data abort, same EL
			auto error = mmuAbortError(*image.code());
			if (updatePageAccess(image, error)) {
				if constexpr (logUpdatePageAccess) {
					infoLogger() << "thor: updated page "
					             << (void *)(*image.faultAddr() & ~(kPageSize - 1))
					             << " status on access from " << (void *)*image.ip() << frg::endlog;
				}

				break;
			}

			handlePageFault(image, *image.faultAddr(), error);
			break;
		}
		case 0x15: // Trapped SVC in AArch64
			handleSyscall(image);
			break;
		case 0x30: // Breakpoint, lower EL
		case 0x31: // Breakpoint, same EL
			handleOtherFault(image, kIntrBreakpoint);
			break;
		case 0x0E: // Illegal Execution fault
		case 0x22: // IP alignment fault
		case 0x26: // SP alignment fault
			handleOtherFault(image, kIntrGeneralFault);
			break;
		case 0x3C: // BRK instruction
			handleOtherFault(image, kIntrBreakpoint);
			break;
		default:
			panicLogger() << "Unexpected fault " << ec << " from ip: " << (void *)*image.ip()
			              << "\n"
			              << "sp: " << (void *)*image.sp() << " "
			              << "syndrome: 0x" << frg::hex_fmt(*image.code()) << " "
			              << "saved state: 0x" << frg::hex_fmt(*image.rflags()) << frg::endlog;
	}

	disableInts();

	// This syscall/fault may have woken up threads on this CPU.
	// See Scheduler::resume() for details.
	checkThreadPreemption(image);
}

extern "C" void onPlatformAsyncFault(FaultImageAccessor image) {
	urgentLogger() << "thor: On CPU " << getCpuData()->cpuIndex << frg::endlog;
	urgentLogger() << "thor: An asynchronous fault has occured!" << frg::endlog;

	auto code = *image.code();
	auto ec = code >> 26;

	bool recoverable = false;

	if (ec == 0x2F) {
		bool ids = code & (1 << 24);
		bool iesb = code & (1 << 13);
		uint8_t aet = (code >> 10) & 7;
		bool ea = code & (1 << 9);
		uint8_t dfsc = code & 0x3F;

		constexpr const char *aet_str[] = {
		    "Uncontainable",
		    "Unrecoverable state",
		    "Restartable state",
		    "Recoverable state",
		    "Reserved",
		    "Reserved",
		    "Corrected",
		    "Reserved"
		};

		if (ids) {
			urgentLogger() << "thor: SError with implementation defined information: ESR = 0x"
			               << frg::hex_fmt{code} << frg::endlog;
		} else {
			auto log = urgentLogger();
			log << "thor: ";

			if (dfsc == 0x11)
				log << aet_str[aet] << " ";

			log << "SError ";

			log << " (EA = " << (ea ? "true" : "false") << ", IESB = " << (iesb ? "true" : "false")
			    << ")";

			if (dfsc != 0x11)
				log << " with DFSC = " << dfsc;

			log << frg::endlog;

			if (aet == 2 || aet == 12)
				recoverable = true;
		}
	} else {
		urgentLogger() << "thor: unexpectec EC " << ec << " (ESR = 0x" << frg::hex_fmt{code} << ")"
		               << frg::endlog;
	}

	urgentLogger() << "thor: IP = 0x" << frg::hex_fmt{*image.ip()} << ", SP = 0x"
	               << frg::hex_fmt{*image.sp()} << frg::endlog;

	if (!recoverable)
		panicLogger() << "thor: Panic due to unrecoverable error" << frg::endlog;
}

extern "C" void onPlatformIrq(IrqImageAccessor image) {
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: IRQ was raised on CPU with no IRQ controller"
		                      << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *) {
		        auto irq = claimGicV2Irq();
		        asm volatile("isb" ::: "memory");
		        handleGicIrq(image, irq);
	        },
	        [&](GicV3 *) {
		        auto irq = claimGicV3Irq();
		        asm volatile("isb" ::: "memory");
		        handleGicIrq(image, irq);
	        },
	    },
	    externalIrq
	);
}

extern "C" void onPlatformWork() {
	assert(!irqMutex().nesting());
	// TODO: User-access should already be disabled here.
	disableUserAccess();

	enableInts();
	getCurrentThread()->mainWorkQueue()->run();
	disableInts();
}

} // namespace thor
