#include <eir-internal/debug.hpp>

enum class IntrType { synchronous, irq, fiq, serror };

extern "C" void eirExceptionHandler(
    IntrType i_type, uintptr_t syndrome, uintptr_t link, uintptr_t state, uintptr_t fault_addr
) {
	eir::infoLogger() << "An unexpected fault has occured:" << frg::endlog;

	const char *i_type_str = "Unknown";
	switch (i_type) {
	case IntrType::synchronous:
		i_type_str = "synchronous";
		break;
	case IntrType::irq:
		i_type_str = "irq";
		break;
	case IntrType::fiq:
		i_type_str = "fiq";
		break;
	case IntrType::serror:
		i_type_str = "SError";
		break;
	}

	eir::infoLogger() << "Interruption type: " << i_type_str << frg::endlog;

	auto exc_type = syndrome >> 26;
	const char *exc_type_str = "Unknown";
	switch (exc_type) {
	case 0x01:
		exc_type_str = "Trapped WFI/WFE";
		break;
	case 0x0e:
		exc_type_str = "Illegal execution";
		break;
	case 0x15:
		exc_type_str = "System call";
		break;
	case 0x20:
		exc_type_str = "Instruction abort, lower EL";
		break;
	case 0x21:
		exc_type_str = "Instruction abort, same EL";
		break;
	case 0x22:
		exc_type_str = "Instruction alignment fault";
		break;
	case 0x24:
		exc_type_str = "Data abort, lower EL";
		break;
	case 0x25:
		exc_type_str = "Data abort, same EL";
		break;
	case 0x26:
		exc_type_str = "Stack alignment fault";
		break;
	case 0x2c:
		exc_type_str = "Floating point";
		break;
	}

	eir::infoLogger() << "Exception type: " << exc_type_str << " (" << (void *)exc_type << ")"
	                  << frg::endlog;

	auto iss = syndrome & ((1 << 25) - 1);

	if (exc_type == 0x25 || exc_type == 0x24) {
		constexpr const char *sas_values[4] = {"Byte", "Halfword", "Word", "Doubleword"};
		constexpr const char *set_values[4] = {
		    "Recoverable", "Uncontainable", "Reserved", "Restartable/Corrected"
		};
		constexpr const char *dfsc_values[4] = {
		    "Address size", "Translation", "Access flag", "Permission"
		};
		eir::infoLogger() << "Access size: " << sas_values[(iss >> 22) & 3] << frg::endlog;
		eir::infoLogger() << "Sign extended? " << (iss & (1 << 21) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Sixty-Four? " << (iss & (1 << 15) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Acquire/Release? " << (iss & (1 << 14) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Synch error type: " << set_values[(iss >> 11) & 3] << frg::endlog;
		eir::infoLogger() << "Fault address valid? " << (iss & (1 << 10) ? "No" : "Yes")
		                  << frg::endlog;
		eir::infoLogger() << "Cache maintenance? " << (iss & (1 << 8) ? "Yes" : "No")
		                  << frg::endlog;
		eir::infoLogger() << "S1PTW? " << (iss & (1 << 7) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Access type: " << (iss & (1 << 6) ? "Write" : "Read") << frg::endlog;
		if ((iss & 0b111111) <= 0b001111)
			eir::infoLogger() << "Data fault status code: " << dfsc_values[(iss >> 2) & 4]
			                  << " fault level " << (iss & 3) << frg::endlog;
		else if ((iss & 0b111111) == 0b10000)
			eir::infoLogger() << "Data fault status code: Synchronous external fault"
			                  << frg::endlog;
		else if ((iss & 0b111111) == 0b100001)
			eir::infoLogger() << "Data fault status code: Alignment fault" << frg::endlog;
		else if ((iss & 0b111111) == 0b110000)
			eir::infoLogger() << "Data fault status code: TLB conflict abort" << frg::endlog;
		else
			eir::infoLogger() << "Data fault status code: unknown" << frg::endlog;
	}

	eir::infoLogger() << "IP: " << (void *)link << ", State: " << (void *)state << frg::endlog;
	eir::infoLogger() << "Syndrome: " << (void *)syndrome
	                  << ", Fault address: " << (void *)fault_addr << frg::endlog;
	eir::infoLogger() << "Halting..." << frg::endlog;

	while (1)
		asm volatile("wfi");
}
