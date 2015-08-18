
#include <stdlib.h>
#include <stdio.h>

#include <acpi.h>

void notImplemented(const char *function) {
	printf("ACPI interface function %s is not implemented!\n", function);
	exit(1);
}

#define NOT_IMPLEMENTED() \
	do { \
		notImplemented(__func__); \
	} while(0)

// --------------------------------------------------------
// Initialization and shutdown
// --------------------------------------------------------

ACPI_STATUS AcpiOsInitialize() {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsTerminate() {
	NOT_IMPLEMENTED();
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer() {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf (const char *format, ...) {
	NOT_IMPLEMENTED();
}

void AcpiOsVprintf (const char *format, va_list args) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Locks
// --------------------------------------------------------

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out_handle) {
	NOT_IMPLEMENTED();
}

void AcpiOsDeleteLock(ACPI_HANDLE handle) {
	NOT_IMPLEMENTED();
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK spinlock) {
	NOT_IMPLEMENTED();
}

void AcpiOsReleaseLock(ACPI_SPINLOCK spinlock, ACPI_CPU_FLAGS flags) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Semaphores
// --------------------------------------------------------

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max_units, UINT32 initial_units,
		ACPI_SEMAPHORE *out_handle) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units,
		UINT16 timeout) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Physical memory access
// --------------------------------------------------------

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS physical, ACPI_SIZE length) {
	NOT_IMPLEMENTED();
}

void AcpiOsUnmapMemory(void *pointer, ACPI_SIZE length) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

void *AcpiOsAllocate(ACPI_SIZE size) {
	NOT_IMPLEMENTED();
}

void AcpiOsFree(void *memory) {
	NOT_IMPLEMENTED();
}

// --------------------------------------------------------
// Threads
// --------------------------------------------------------

ACPI_THREAD_ID AcpiOsGetThreadId() {
	NOT_IMPLEMENTED();
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
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_TABLE_HEADER **new_table) {
	NOT_IMPLEMENTED();
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing,
		ACPI_PHYSICAL_ADDRESS *new_address, UINT32 *new_length) {
	NOT_IMPLEMENTED();
}

