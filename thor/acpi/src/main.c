
#include <stdlib.h>
#include <stdio.h>

#include <acpi.h>

int main() {
	ACPI_STATUS status;
	status = AcpiInitializeSubsystem();
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeSubsystem() failed!\n");
		exit(1);
	}

	status = AcpiInitializeTables(NULL, 16, FALSE);
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeTables() failed!\n");
		exit(1);
	}

	status = AcpiLoadTables();
	if(ACPI_FAILURE(status)) {
		printf("AcpiLoadTables() failed!\n");
		exit(1);
	}
	
	status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
	if(ACPI_FAILURE(status)) {
		printf("AcpiEnableSubsystem() failed!\n");
		exit(1);
	}
	
	status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
	if(ACPI_FAILURE(status)) {
		printf("AcpiInitializeObjects() failed!\n");
		exit(1);
	}
}


