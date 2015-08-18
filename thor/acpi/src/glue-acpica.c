
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <hel.h>
#include <hel-syscalls.h>

#include <acpi.h>

void notImplemented(const char *function) {
	printf("ACPI interface function %s is not implemented!\n", function);
	abort();
}

#define NOT_IMPLEMENTED() \
	do { \
		notImplemented(__func__); \
	} while(0)

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
	// TODO: implement at least a counter
	return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle) {
	// TODO: implement at least a counter
	return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units) {
	// TODO: implement at least a counter
	return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units,
		UINT16 timeout) {
	// TODO: implement at least a counter
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
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width) {
	NOT_IMPLEMENTED();
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
	*new_address = NULL;
	return AE_OK;
}

