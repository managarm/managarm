
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <hel.h>
#include <hel-syscalls.h>
#include <thor.h>

#include <acpi.h>

struct ApicHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct ApicEntry {
	uint8_t type;
	uint8_t length;
};

struct ApicLocalEntry {
	struct ApicEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

int main() {
	ACPI_STATUS status;
	status = AcpiInitializeSubsystem();
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeSubsystem() failed!\n");
		abort();
	}

	status = AcpiInitializeTables(NULL, 16, FALSE);
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeTables() failed!\n");
		abort();
	}

	status = AcpiLoadTables();
	if(ACPI_FAILURE(status)) {
		printf("AcpiLoadTables() failed!\n");
		abort();
	}
	
	status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
	if(ACPI_FAILURE(status)) {
		printf("AcpiEnableSubsystem() failed!\n");
		abort();
	}
	
	status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeObjects() failed!\n");
		abort();
	}

	printf("ACPI initialized successfully\n");

	ACPI_TABLE_HEADER *table;
	if(AcpiGetTable("APIC", 0, &table) != AE_OK) {
		printf("Could not find MADT\n");
		abort();
	}
	printf("MADT length: %u\n", table->Length);

	int seen_bsp = 0;

	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(struct ApicHeader);
	while(offset < table->Length) {
		struct ApicEntry *generic = (struct ApicEntry *)((uintptr_t)table + offset);
		printf("APIC: %d (length %d)\n", generic->type, generic->length);
		
		if(generic->type == 0) { // local APIC
			struct ApicLocalEntry *entry = (struct ApicLocalEntry *)generic;
			printf("Local APIC id: %d\n", entry->localApicId);

			if(seen_bsp)
				helControlKernel(kThorSubArch, kThorIfBootSecondary,
						&entry->localApicId, NULL);
			seen_bsp = 1;
		}
		offset += generic->length;
	}
}


