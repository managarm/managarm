
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <hel.h>
#include <hel-syscalls.h>

extern "C" {
#include <acpi.h>
}

#define NOT_IMPLEMENTED() do { printf("ACPI interface function %s is not implemented!\n", \
		__func__); abort(); } while(0)

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
	if(AcpiFindRootPointer(&pointer) != AE_OK) {
		printf("Could not find ACPI RSDP table");
		abort();
	}
	return pointer;
}

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf (const char *format, ...) {
	va_list args;
	va_start(args, format);
	AcpiOsVprintf(format, args);
	va_end(args);
}

void AcpiOsVprintf (const char *format, va_list args) {
	vprintf(format, args);
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
	helAccessPhysical(physical, length, &memory);

	void *actual_pointer;
	helMapMemory(memory, kHelNullHandle, NULL, length,
			kHelMapReadWrite, &actual_pointer);
	return (void *)((uintptr_t)actual_pointer + alignment);
}

void AcpiOsUnmapMemory(void *pointer, ACPI_SIZE length) {
	// TODO: implement this
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

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 interrupt,
		ACPI_OSD_HANDLER handler, void *context) {
	printf("Handle int %d\n", interrupt);
	//NOT_IMPLEMENTED();
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
		// enable the I/O port
		uintptr_t base = address;
		HelHandle handle;
		HEL_CHECK(helAccessIo(&base, 1, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint8_t result;
		asm volatile ( "inb %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else if(width == 16) {
		// enable the I/O port
		uintptr_t array[2] = { address, address + 1 };
		HelHandle handle;
		HEL_CHECK(helAccessIo(array, 2, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint16_t result;
		asm volatile ( "inw %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else if(width == 32) {
		// enable the I/O port
		uintptr_t array[4] = { address, address + 1, address + 2, address + 3 };
		HelHandle handle;
		HEL_CHECK(helAccessIo(array, 4, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint32_t result;
		asm volatile ( "inl %1, %0" : "=a"(result) : "d"(port) );
		*value = result;
	}else{
		printf("Unexpected bit width for AcpiOsReadPort()");
		abort();
	}
	
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width) {
	if(width == 8) {
		// enable the I/O port
		uintptr_t base = address;
		HelHandle handle;
		HEL_CHECK(helAccessIo(&base, 1, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint8_t to_write = value;
		asm volatile ( "outb %0, %1" : : "a"(to_write), "d"(port) );
	}else if(width == 16) {
		// enable the I/O port
		uintptr_t array[2] = { address, address + 1 };
		HelHandle handle;
		HEL_CHECK(helAccessIo(array, 2, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint16_t to_write = value;
		asm volatile ( "outw %0, %1" : : "a"(to_write), "d"(port) );
	}else if(width == 32) {
		// enable the I/O port
		uintptr_t array[4] = { address, address + 1, address + 2, address + 3 };
		HelHandle handle;
		HEL_CHECK(helAccessIo(array, 4, &handle));
		HEL_CHECK(helEnableIo(handle));

		// read the I/O port
		uint16_t port = address;
		uint32_t to_write = value;
		asm volatile ( "outl %0, %1" : : "a"(to_write), "d"(port) );
	}else{
		printf("Unexpected bit width for AcpiOsWritePort()");
		abort();
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
	*new_value = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_TABLE_HEADER **new_table) {
	*new_table = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_PHYSICAL_ADDRESS *new_address, UINT32 *new_length) {
	*new_address = 0;
	return AE_OK;
}

