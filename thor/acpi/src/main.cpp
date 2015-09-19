
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/libc.hpp>
#include <frigg/initializer.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>
#include <thor.h>

extern "C" {
#include <acpi.h>
}

struct GenericAddress {
	uint8_t space;
	uint8_t bitWidth;
	uint8_t bitOffset;
	uint8_t size;
	uint64_t offset;
} __attribute__ (( packed ));

struct MadtHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct MadtGenericEntry {
	uint8_t type;
	uint8_t length;
};

struct MadtLocalEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

struct MadtIoEntry {
	MadtGenericEntry generic;
	uint8_t ioApicId;
	uint8_t reserved;
	uint32_t mmioAddress;
	uint32_t systemIntBase;
};

struct MadtIntOverrideEntry {
	MadtGenericEntry generic;
	uint8_t bus;
	uint8_t sourceIrq;
	uint32_t systemInt;
	uint16_t flags;
};

struct MadtLocalNmiEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint16_t flags;
	uint8_t localInt;
} __attribute__ (( packed ));

struct HpetEntry {
	uint32_t generalCapsAndId;
	GenericAddress address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

void acpicaCheckFailed(const char *expr, const char *file, int line) {
	frigg::debug::panicLogger.log() << "ACPICA_CHECK failed: "
			<< expr << "\nIn file " << file << " on line " << line
			<< frigg::debug::Finish();
}

#define ACPICA_CHECK(expr) do { if((expr) != AE_OK) { \
		acpicaCheckFailed(#expr, __FILE__, __LINE__); } } while(0)

/*void findChildrenByType(ACPI_HANDLE parent, ACPI_OBJECT_TYPE type,
		std::vector<ACPI_HANDLE> &results) {
	ACPI_HANDLE previous = nullptr;
	while(true) {
		ACPI_HANDLE child;
		ACPI_STATUS status = AcpiGetNextObject(type, parent, previous, &child);
		if(status == AE_NOT_FOUND)
			break;
		ACPICA_CHECK(status);
		
		results.push_back(child);
		previous = child;
	}
}

void dumpNamespace(ACPI_HANDLE object, int depth) {
	ACPI_OBJECT_TYPE type;
	ACPICA_CHECK(AcpiGetType(object, &type));

	char segment[5];
	ACPI_BUFFER buffer;
	buffer.Pointer = segment;
	buffer.Length = 5;
	ACPICA_CHECK(AcpiGetName(object, ACPI_SINGLE_NAME, &buffer));
	
	for(int i = 0; i < depth; i++)
		printf("    ");
	if(type == ACPI_TYPE_DEVICE) {
		printf("Device: ");
	}else if(type == ACPI_TYPE_PROCESSOR) {
		printf("Processor: ");
	}else if(type == ACPI_TYPE_MUTEX) {
		printf("Mutex: ");
	}else if(type == ACPI_TYPE_LOCAL_SCOPE) {
		printf("Scope: ");
	}else{
		printf("(Unknown type 0x%X): ", type);
	}
	printf("%s\n", segment);
	
	std::vector<ACPI_HANDLE> methods;
	findChildrenByType(object, ACPI_TYPE_METHOD, methods);
	if(!methods.empty()) {
		for(int i = 0; i < depth; i++)
			printf("    ");
		printf("    Methods: ");
		for(auto it = methods.begin(); it != methods.end(); ++it) {
			char method_name[5];
			ACPI_BUFFER method_buffer;
			method_buffer.Pointer = method_name;
			method_buffer.Length = 5;
			ACPICA_CHECK(AcpiGetName(*it, ACPI_SINGLE_NAME, &method_buffer));

			printf(" %s", method_name);
		}
		printf("\n");
	}
	
	std::vector<ACPI_HANDLE> literals;
	findChildrenByType(object, ACPI_TYPE_INTEGER, literals);
	findChildrenByType(object, ACPI_TYPE_STRING, literals);
	findChildrenByType(object, ACPI_TYPE_BUFFER, literals);
	findChildrenByType(object, ACPI_TYPE_PACKAGE, literals);
	if(!literals.empty()) {
		for(int i = 0; i < depth; i++)
			printf("    ");
		printf("    Literals: ");
		for(auto it = literals.begin(); it != literals.end(); ++it) {
			char literal_name[5];
			ACPI_BUFFER literal_buffer;
			literal_buffer.Pointer = literal_name;
			literal_buffer.Length = 5;
			ACPICA_CHECK(AcpiGetName(*it, ACPI_SINGLE_NAME, &literal_buffer));

			printf(" %s", literal_name);
		}
		printf("\n");
	}

	std::vector<ACPI_HANDLE> children;
	findChildrenByType(object, ACPI_TYPE_ANY, children);
	for(auto it = children.begin(); it != children.end(); ++it) {
		ACPI_OBJECT_TYPE child_type;
		ACPICA_CHECK(AcpiGetType(*it, &child_type));
		
		if(child_type != ACPI_TYPE_METHOD
				&& child_type != ACPI_TYPE_INTEGER
				&& child_type != ACPI_TYPE_STRING
				&& child_type != ACPI_TYPE_BUFFER
				&& child_type != ACPI_TYPE_PACKAGE)
			dumpNamespace(*it, depth + 1);
	}
}*/

void pciDiscover(); // TODO: put this in a header file


typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	infoLogger.initialize(infoSink);
	allocator.initialize(virtualAlloc);

	ACPICA_CHECK(AcpiInitializeSubsystem());
	ACPICA_CHECK(AcpiInitializeTables(nullptr, 16, FALSE));
	ACPICA_CHECK(AcpiLoadTables());
	ACPICA_CHECK(AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION));
	ACPICA_CHECK(AcpiInitializeObjects(ACPI_FULL_INITIALIZATION));
	infoLogger->log() << "ACPI initialized successfully" << frigg::debug::Finish();
	
	// initialize the hpet
	ACPI_TABLE_HEADER *hpet_table;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("HPET"), 0, &hpet_table));

	auto hpet_entry = (HpetEntry *)((uintptr_t)hpet_table + sizeof(ACPI_TABLE_HEADER));
	helControlKernel(kThorSubArch, kThorIfSetupHpet, &hpet_entry->address.offset, nullptr);

	// boot secondary processors
	ACPI_TABLE_HEADER *madt_table;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("APIC"), 0, &madt_table));
	
	int seen_bsp = 0;

	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt_table->Length) {
		auto generic = (MadtGenericEntry *)((uintptr_t)madt_table + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			infoLogger->log() << "    Local APIC id: "
					<< entry->localApicId << frigg::debug::Finish();

			if(seen_bsp)
				helControlKernel(kThorSubArch, kThorIfBootSecondary,
						&entry->localApicId, nullptr);
			seen_bsp = 1;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			infoLogger->log() << "    I/O APIC id: " << entry->ioApicId
					<< ", sytem interrupt base: " << entry->systemIntBase
					<< frigg::debug::Finish();
			
			uint64_t address = entry->mmioAddress;
			helControlKernel(kThorSubArch, kThorIfSetupIoApic, &address, nullptr);
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;
			infoLogger->log() << "    Int override: bus " << entry->bus
					<< ", irq " << entry->sourceIrq << " -> " << entry->systemInt
					<< frigg::debug::Finish();
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			infoLogger->log() << "    Local APIC NMI: processor " << entry->processorId
					<< ", lint: " << entry->localInt << frigg::debug::Finish();
		}else{
			infoLogger->log() << "    Unexpected MADT entry of type "
					<< generic->type << frigg::debug::Finish();
		}
		offset += generic->length;
	}
	helControlKernel(kThorSubArch, kThorIfFinishBoot, nullptr, nullptr);
	
//	dumpNamespace(ACPI_ROOT_OBJECT, 0);

//	pciDiscover();

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));

	HEL_CHECK(helExitThisThread());
}

asm ( ".global _start\n"
		"_start:\n"
		"\tcall main\n"
		"\tud2" );

