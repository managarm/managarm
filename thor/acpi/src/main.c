
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <hel.h>
#include <hel-syscalls.h>
#include <thor.h>

#include <acpi.h>

struct GenericAddress {
	uint8_t space;
	uint8_t bitWidth;
	uint8_t bitOffset;
	uint8_t size;
	uint64_t offset;
} __attribute__ (( packed ));

struct ApicHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct ApicGenericEntry {
	uint8_t type;
	uint8_t length;
};

struct ApicLocalEntry {
	struct ApicGenericEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

struct HpetEntry {
	uint32_t generalCapsAndId;
	struct GenericAddress address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

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
	
	// initialize the hpet
	ACPI_TABLE_HEADER *hpet_table;
	if(AcpiGetTable("HPET", 0, &hpet_table) != AE_OK) {
		printf("Could not find HPET\n");
		abort();
	}
	printf("HPET length: %u\n", hpet_table->Length);

	struct HpetEntry *hpet_entry = (struct HpetEntry *)((uintptr_t)hpet_table
			+ sizeof(ACPI_TABLE_HEADER));
	
	helControlKernel(kThorSubArch, kThorIfSetupHpet,
			&hpet_entry->address.offset, NULL);

	// boot secondary processors
	ACPI_TABLE_HEADER *madt_table;
	if(AcpiGetTable("APIC", 0, &madt_table) != AE_OK) {
		printf("Could not find MADT\n");
		abort();
	}
	printf("MADT length: %u\n", madt_table->Length);
	
	int seen_bsp = 0;

	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(struct ApicHeader);
	while(offset < madt_table->Length) {
		struct ApicGenericEntry *generic = (struct ApicGenericEntry *)((uintptr_t)madt_table
				+ offset);
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


