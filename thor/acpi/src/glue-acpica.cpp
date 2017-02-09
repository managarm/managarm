
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <iostream>
#include <stdexcept>

#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>

extern "C" {
#include <acpi.h>
}

#define NOT_IMPLEMENTED() do { assert(!"Fix this"); /* frigg::panicLogger() << "ACPI interface function " << __func__ << " is not implemented!" << frigg::endLog;*/ } while(0)

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
		throw std::runtime_error("Could not find ACPI RSDP table");
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
	if(vprintf(format, args))
		throw std::runtime_error("vprintf failed");
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
	auto semaphore = new AcpiSemaphore;
	semaphore->counter = initial_units;
	*out_handle = semaphore;
	return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle) {
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
	ACPI_SIZE alignment = physical % 0x1000;
	physical -= alignment;
	length += alignment;

	if((length % 0x1000) != 0)
		length += 0x1000 - (length % 0x1000);

	HelHandle memory;
	HEL_CHECK(helAccessPhysical(physical, length, &memory));

	void *actual_pointer;
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, NULL, 0, length,
			kHelMapReadWrite, &actual_pointer));
	HEL_CHECK(helCloseDescriptor(memory));

	return (void *)((uintptr_t)actual_pointer + alignment);
}

void AcpiOsUnmapMemory(void *pointer, ACPI_SIZE length) {
	uintptr_t address = (uintptr_t)pointer;
	ACPI_SIZE alignment = address % 0x1000;
	address -= alignment;
	length += alignment;

	if((length % 0x1000) != 0)
		length += 0x1000 - (length % 0x1000);
	
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

void *AcpiOsAllocate(ACPI_SIZE size) {
	return malloc(size);
}

void AcpiOsFree(void *pointer) {
	free(pointer);
}

// --------------------------------------------------------
// Interrupts
// --------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, listenForInts(unsigned int number,
		uint32_t (*handler)(void *), void *context), ([=] {
	std::cout << "ACPI: Installing handler for IRQ " << number << std::endl;

	HelHandle handle;
	HEL_CHECK(helAccessIrq(number, &handle));
	helix::UniqueIrq irq{handle};

	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(irq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());

		std::cout << "ACPI: Running IRQ handler" << std::endl;

		if(handler(context) == ACPI_INTERRUPT_HANDLED)
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle()));
	}
}))

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 number,
		ACPI_OSD_HANDLER handler, void *context) {
	listenForInts(number, handler, context);
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

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *pci_id, UINT32 register_num,
		UINT64 *value, UINT32 width) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *pci_id, UINT32 register_num,
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

