
#include <frigg/debug.hpp>
#include <frigg/printf.hpp>
#include <frigg/memory.hpp>
#include "../../arch/x86/paging.hpp"
#include "../../arch/x86/pic.hpp"
#include "../../generic/irq.hpp"
#include "../../generic/kernel_heap.hpp"
#include "../../system/pci/pci.hpp"

extern "C" {
#include <acpi.h>
}

#define NOT_IMPLEMENTED() do { assert(!"Fix this"); /* frigg::panicLogger() << "ACPI interface function " << __func__ << " is not implemented!" << frigg::endLog;*/ } while(0)

using namespace thor;

// --------------------------------------------------------
// Initialization and shutdown
// --------------------------------------------------------

ACPI_STATUS AcpiOsInitialize() {
	return AE_OK;
}

ACPI_STATUS AcpiOsTerminate() {
	return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer() {
	ACPI_SIZE pointer;
	if(AcpiFindRootPointer(&pointer) != AE_OK)
		frigg::panicLogger() << "thor: Could not find ACPI RSDP table" << frigg::endLog;
	return pointer;
}

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *format, ...) {
	va_list args;
	va_start(args, format);
	AcpiOsVprintf(format, args);
	va_end(args);
}

void AcpiOsVprintf(const char *format, va_list args) {
	auto printer = frigg::infoLogger();
//	frigg::infoLogger() << "printf: " << format << frigg::endLog;
	frigg::printf(printer, format, args);
//	TODO: Call finish()?
//	printer.finish();
}

// --------------------------------------------------------
// Locks
// --------------------------------------------------------

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out_handle) {
	// TODO: implement this
	return AE_OK;
}

void AcpiOsDeleteLock(ACPI_HANDLE handle) {
	// TODO: implement this
}

// this function should disable interrupts
ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK spinlock) {
	// TODO: implement this
	return 0;
}

// this function should re-enable interrupts
void AcpiOsReleaseLock(ACPI_SPINLOCK spinlock, ACPI_CPU_FLAGS flags) {
	// TODO: implement this
}

// --------------------------------------------------------
// Semaphores
// --------------------------------------------------------

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max_units, UINT32 initial_units,
		ACPI_SEMAPHORE *out_handle) {
	auto semaphore = frigg::construct<AcpiSemaphore>(*kernelAlloc);
	semaphore->counter = initial_units;
	*out_handle = semaphore;
	return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle) {
	NOT_IMPLEMENTED();
	return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units) {
	assert(units == 1);
	handle->counter++;
	return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units, UINT16 timeout) {
	assert(units == 1);
	assert(handle->counter > 0);
	handle->counter--;
	return AE_OK;
}

// --------------------------------------------------------
// Physical memory access
// --------------------------------------------------------

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS physical, ACPI_SIZE length) {
	auto paddr = physical & ~(kPageSize - 1);
	auto vsize = length + (physical & (kPageSize - 1));
	assert(vsize <= 0x100000);

	auto ptr = KernelVirtualMemory::global().allocate(0x100000);
	for(size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k((VirtualAddr)ptr + pg, paddr + pg,
				page_access::write);
	return reinterpret_cast<char *>(ptr) + (physical & (kPageSize - 1));
}

void AcpiOsUnmapMemory(void *pointer, ACPI_SIZE length) {
	auto vaddr = (uintptr_t)pointer & ~(kPageSize - 1);
	auto vsize = length + ((uintptr_t)pointer & (kPageSize - 1));
	assert(vsize <= 0x100000);

	for(size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().unmapSingle4k(vaddr + pg);
//TODO:	KernelVirtualMemory::global().free(pointer);
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

void *AcpiOsAllocate(ACPI_SIZE size) {
	return kernelAlloc->allocate(size);
}

void AcpiOsFree(void *pointer) {
	kernelAlloc->free(pointer);
}

// --------------------------------------------------------
// Interrupts
// --------------------------------------------------------

namespace {
	struct AcpiSink : IrqSink {
		AcpiSink(ACPI_OSD_HANDLER handler, void *context)
		: _handler{handler}, _context{context} { }

		IrqStatus raise() override {
			auto report = [] (unsigned int event, const char *name) {
				ACPI_EVENT_STATUS status;
				AcpiGetEventStatus(event, &status);
				const char *enabled = (status & ACPI_EVENT_FLAG_ENABLED) ? "enabled" : "disabled";
				const char *set = (status & ACPI_EVENT_FLAG_SET) ? "set" : "clear";
				frigg::infoLogger() << "    " << name << ": " << enabled
						<< " " << set << frigg::endLog;
			};

			frigg::infoLogger() << "thor: Handling ACPI interrupt." << frigg::endLog;
			report(ACPI_EVENT_PMTIMER, "ACPI timer");
			report(ACPI_EVENT_GLOBAL, "Global lock");
			report(ACPI_EVENT_POWER_BUTTON, "Power button");
			report(ACPI_EVENT_SLEEP_BUTTON, "Sleep button");
			report(ACPI_EVENT_RTC, "RTC");

			auto result = _handler(_context);
			if(result == ACPI_INTERRUPT_HANDLED) {
				getPin()->acknowledge();
				return irq_status::handled;
			}else{
				assert(result == ACPI_INTERRUPT_NOT_HANDLED);
				return irq_status::null;
			}
		}

	private:
		ACPI_OSD_HANDLER _handler;
		void *_context;
	};
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 number,
		ACPI_OSD_HANDLER handler, void *context) {
	frigg::infoLogger() << "thor: Installing handler for ACPI IRQ " << number << frigg::endLog;

	auto sink = frigg::construct<AcpiSink>(*kernelAlloc, handler, context);
	auto pin = getGlobalSystemIrq(number);
	attachIrq(pin, sink);
	return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 interrupt,
		ACPI_OSD_HANDLER handler) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Threads
// --------------------------------------------------------

ACPI_THREAD_ID AcpiOsGetThreadId() {
	return 1;
}

void AcpiOsSleep(UINT64 milliseconds) {
	NOT_IMPLEMENTED();
}

void AcpiOsStall(UINT32 milliseconds) {
	NOT_IMPLEMENTED();
}

UINT64 AcpiOsGetTimer() {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsSignal(UINT32 function, void *info) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Async execution
// --------------------------------------------------------

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type,
		ACPI_OSD_EXEC_CALLBACK function, void *context) {
	NOT_IMPLEMENTED();
}

void AcpiOsWaitEventsComplete() {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Hardware access
// --------------------------------------------------------

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address,
		UINT64 *value, UINT32 width) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address,
		UINT64 value, UINT32 width) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS address, UINT32 *value, UINT32 width) {
	if(width == 8) {
		// read the I/O port
		uint16_t port = address;
		uint8_t result;
		asm volatile ( "inb %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else if(width == 16) {
		// read the I/O port
		uint16_t port = address;
		uint16_t result;
		asm volatile ( "inw %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else if(width == 32) {
		// read the I/O port
		uint16_t port = address;
		uint32_t result;
		asm volatile ( "inl %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else{
		assert(!"Unexpected bit width for AcpiOsReadPort()");
	}
	
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width) {
	if(width == 8) {
		// read the I/O port
		uint16_t port = address;
		uint8_t to_write = value;
		asm volatile ( "outb %0, %1" : : "a"(to_write), "d"(port) );
	}else if(width == 16) {
		// read the I/O port
		uint16_t port = address;
		uint16_t to_write = value;
		asm volatile ( "outw %0, %1" : : "a"(to_write), "d"(port) );
	}else if(width == 32) {
		// read the I/O port
		uint16_t port = address;
		uint32_t to_write = value;
		asm volatile ( "outl %0, %1" : : "a"(to_write), "d"(port) );
	}else{
		assert(!"Unexpected bit width for AcpiOsWritePort()");
	}
	
	return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *target, UINT32 offset,
		UINT64 *value, UINT32 width) {
/*	std::cout << "segment: " << target->Segment
			<< ", bus: " << target->Bus
			<< ", slot: " << target->Device
			<< ", function: " << target->Function << std::endl;*/

	assert(!target->Segment);
	switch(width) {
	case 8:
		*value = readPciByte(target->Bus, target->Device, target->Function, offset);
		break;
	case 16:
		*value = readPciHalf(target->Bus, target->Device, target->Function, offset);
		break;
	case 32:
		*value = readPciWord(target->Bus, target->Device, target->Function, offset);
		break;
	default:
		frigg::panicLogger() << "Unexpected PCI access width" << frigg::endLog;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *target, UINT32 offset,
		UINT64 value, UINT32 width) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Table / object override
// --------------------------------------------------------

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *predefined,
		ACPI_STRING *new_value) {
	*new_value = nullptr;
	return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_TABLE_HEADER **new_table) {
	*new_table = nullptr;
	return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_PHYSICAL_ADDRESS *new_address, UINT32 *new_length) {
	*new_address = 0;
	return AE_OK;
}

