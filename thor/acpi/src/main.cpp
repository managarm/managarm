
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/debug.hpp>
#include <frigg/libc.hpp>
#include <frigg/initializer.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/glue-hel.hpp>
#include <frigg/chain-all.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>
#include <thor.h>

extern "C" {
#include <acpi.h>
}

#include "common.hpp"
#include <mbus.frigg_pb.hpp>

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
	frigg::panicLogger.log() << "ACPICA_CHECK failed: "
			<< expr << "\nIn file " << file << " on line " << line
			<< frigg::EndLog();
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

helx::EventHub eventHub = helx::EventHub::create();
helx::Pipe mbusPipe;

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure {
	void operator() ();

private:
	void recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	HEL_CHECK(mbusPipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &MbusClosure::recvdRequest)));
}

void MbusClosure::recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::mbus::SvrReqType::REQUIRE_IF) {
		helx::Pipe local, remote;
		helx::Pipe::createFullPipe(local, remote);
		requireObject(request.object_id(), frigg::move(local));
		
		auto action = mbusPipe.sendDescriptorResp(remote.getHandle(), eventHub, msg_request, 1)
		+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		
		frigg::run(frigg::move(action), allocator.get());
	}

	(*this)();
}

// --------------------------------------------------------
// main()
// --------------------------------------------------------

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ACPI driver" << frigg::EndLog();
	allocator.initialize(virtualAlloc);
	
	// connect to mbus
	const char *mbus_path = "local/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	helx::Client mbus_client(mbus_handle);
	HelError mbus_error;
	mbus_client.connectSync(eventHub, mbus_error, mbusPipe);
	HEL_CHECK(mbus_error);
	mbus_client.reset();
	
	frigg::runClosure<MbusClosure>(*allocator);

	// initialize the ACPI subsystem
	HEL_CHECK(helEnableFullIo());

	ACPICA_CHECK(AcpiInitializeSubsystem());
	ACPICA_CHECK(AcpiInitializeTables(nullptr, 16, FALSE));
	ACPICA_CHECK(AcpiLoadTables());
	ACPICA_CHECK(AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION));
	ACPICA_CHECK(AcpiInitializeObjects(ACPI_FULL_INITIALIZATION));
	infoLogger->log() << "ACPI initialized successfully" << frigg::EndLog();
	
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
					<< entry->localApicId << frigg::EndLog();

			if(seen_bsp)
				helControlKernel(kThorSubArch, kThorIfBootSecondary,
						&entry->localApicId, nullptr);
			seen_bsp = 1;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			infoLogger->log() << "    I/O APIC id: " << entry->ioApicId
					<< ", sytem interrupt base: " << entry->systemIntBase
					<< frigg::EndLog();
			
			uint64_t address = entry->mmioAddress;
			helControlKernel(kThorSubArch, kThorIfSetupIoApic, &address, nullptr);
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;
			infoLogger->log() << "    Int override: bus " << entry->bus
					<< ", irq " << entry->sourceIrq << " -> " << entry->systemInt
					<< frigg::EndLog();
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			infoLogger->log() << "    Local APIC NMI: processor " << entry->processorId
					<< ", lint: " << entry->localInt << frigg::EndLog();
		}else{
			infoLogger->log() << "    Unexpected MADT entry of type "
					<< generic->type << frigg::EndLog();
		}
		offset += generic->length;
	}
	helControlKernel(kThorSubArch, kThorIfFinishBoot, nullptr, nullptr);
	
//	dumpNamespace(ACPI_ROOT_OBJECT, 0);

	pciDiscover();

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
/*
	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0, kHelRequest));
	HEL_CHECK(helCloseDescriptor(parent_handle));
*/
	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	
	helx::Pipe parent_pipe(parent_handle);
	HelError send_error;
	parent_pipe.sendDescriptorSync(client.getHandle(), eventHub,
			0, 0, kHelRequest, send_error);	
	HEL_CHECK(send_error);
	parent_pipe.reset();
	client.reset();

	while(true)
		eventHub.defaultProcessEvents();
}

asm ( ".global _start\n"
		"_start:\n"
		"\tint3\n"
		"\tcall main\n"
		"\tud2" );

extern "C"
int __cxa_atexit(void (*func) (void *), void *arg, void *dso_handle) {
	return 0;
}

void *__dso_handle;

