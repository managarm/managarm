
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <iostream>
#include <vector>

#include <helix/ipc.hpp>
#include <thor.h>

extern "C" {
#include <acpi.h>
}

#include "common.hpp"

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
	assert(!"Fix this");
/*	frigg::panicLogger() << "ACPICA_CHECK failed: "
			<< expr << "\nIn file " << file << " on line " << line
			<< frigg::endLog;*/
}

#define ACPICA_CHECK(expr) do { if((expr) != AE_OK) { \
		acpicaCheckFailed(#expr, __FILE__, __LINE__); } } while(0)

void findChildrenByType(ACPI_HANDLE parent, ACPI_OBJECT_TYPE type,
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
	ACPI_BUFFER name_buffer;
	name_buffer.Pointer = segment;
	name_buffer.Length = 5;
	ACPICA_CHECK(AcpiGetName(object, ACPI_SINGLE_NAME, &name_buffer));

	assert(!"Fix this");
/*	auto log_type = frigg::infoLogger();
	for(int i = 0; i < depth; i++)
		log_type << "    ";
	if(type == ACPI_TYPE_DEVICE) {
		log_type << "Device: ";
	}else if(type == ACPI_TYPE_MUTEX) {
		log_type << "Mutex: ";
	}else if(type == ACPI_TYPE_REGION) {
		log_type << "Region: ";
	}else if(type == ACPI_TYPE_PROCESSOR) {
		log_type << "Processor: ";
	}else if(type == ACPI_TYPE_LOCAL_SCOPE) {
		log_type << "Scope: ";
	}else{
		log_type << "(Unknown type 0x" << frigg::logHex(type) << ") ";
	}
	log_type << (const char *)segment << frigg::endLog;
	
	if(strcmp(segment, "PCI0") == 0) {
		ACPI_BUFFER rt_buffer;
		rt_buffer.Pointer = nullptr;
		rt_buffer.Length = ACPI_ALLOCATE_BUFFER;

		ACPICA_CHECK(AcpiGetIrqRoutingTable(object, &rt_buffer));
		frigg::infoLogger() << "Routing table:" << frigg::endLog;

		size_t offset = 0;
		while(true) {
			assert(offset < rt_buffer.Length);
			auto entry = (ACPI_PCI_ROUTING_TABLE *)((char *)rt_buffer.Pointer + offset);
			if(entry->Length == 0)
				break;
			frigg::infoLogger() << "Pin: " << entry->Pin
					<< ", source: " << (const char *)entry->Source << frigg::endLog;
			offset += entry->Length;
		}

		AcpiOsFree(rt_buffer.Pointer);
	}
	
	frigg::Vector<ACPI_HANDLE, Allocator> methods(*allocator);
	findChildrenByType(object, ACPI_TYPE_METHOD, methods);
	if(!methods.empty()) {
		auto log_methods = frigg::infoLogger();
		for(int i = 0; i < depth; i++)
			log_methods << "    ";
		log_methods << "    Methods: ";
		for(size_t i = 0; i < methods.size(); i++) {
			char method_name[5];
			ACPI_BUFFER method_buffer;
			method_buffer.Pointer = method_name;
			method_buffer.Length = 5;
			ACPICA_CHECK(AcpiGetName(methods[i], ACPI_SINGLE_NAME, &method_buffer));

			log_methods << (const char *)method_name << " ";
		}
		log_methods << frigg::endLog;
	}
	
	frigg::Vector<ACPI_HANDLE, Allocator> literals(*allocator);
	findChildrenByType(object, ACPI_TYPE_INTEGER, literals);
	findChildrenByType(object, ACPI_TYPE_STRING, literals);
	findChildrenByType(object, ACPI_TYPE_BUFFER, literals);
	findChildrenByType(object, ACPI_TYPE_PACKAGE, literals);
	if(!literals.empty()) {
		auto log_literals = frigg::infoLogger();
		for(int i = 0; i < depth; i++)
			log_literals << "    ";
		log_literals << "    Literals: ";
		for(size_t i = 0; i < literals.size(); i++) {
			char literal_name[5];
			ACPI_BUFFER literal_buffer;
			literal_buffer.Pointer = literal_name;
			literal_buffer.Length = 5;
			ACPICA_CHECK(AcpiGetName(literals[i], ACPI_SINGLE_NAME, &literal_buffer));

			log_literals << (const char *)literal_name << " ";
		}
		log_literals << frigg::endLog;
	}

	frigg::Vector<ACPI_HANDLE, Allocator> children(*allocator);
	findChildrenByType(object, ACPI_TYPE_ANY, children);
	for(size_t i = 0; i < children.size(); i++) {
		ACPI_OBJECT_TYPE child_type;
		ACPICA_CHECK(AcpiGetType(children[i], &child_type));
		
		if(child_type != ACPI_TYPE_METHOD
				&& child_type != ACPI_TYPE_INTEGER
				&& child_type != ACPI_TYPE_STRING
				&& child_type != ACPI_TYPE_BUFFER
				&& child_type != ACPI_TYPE_PACKAGE)
			dumpNamespace(children[i], depth + 1);
	}*/
}

void pciDiscover(); // TODO: put this in a header file

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

/*struct MbusClosure {
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
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		
		frigg::run(frigg::move(action), allocator.get());
	}

	(*this)();
}*/

// --------------------------------------------------------
// main()
// --------------------------------------------------------

int main() {
	std::cout << "Entering ACPI driver" << std::endl;
	
	// connect to mbus
	/*HelError mbus_recv_error;
	HelHandle mbus_handle;
	//FIXME superior.recvDescriptorRespSync(eventHub, 1001, 0, mbus_recv_error, mbus_handle);
	HEL_CHECK(mbus_recv_error);

	helx::Client mbus_client(mbus_handle);
	HelError mbus_connect_error;
	mbus_client.connectSync(eventHub, mbus_connect_error, mbusPipe);
	
	frigg::runClosure<MbusClosure>(*allocator);*/

	// initialize the ACPI subsystem
	HEL_CHECK(helEnableFullIo());

	ACPICA_CHECK(AcpiInitializeSubsystem());
	ACPICA_CHECK(AcpiInitializeTables(nullptr, 16, FALSE));
	ACPICA_CHECK(AcpiLoadTables());
	ACPICA_CHECK(AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION));
	ACPICA_CHECK(AcpiInitializeObjects(ACPI_FULL_INITIALIZATION));
	std::cout << "ACPI initialized successfully" << std::endl;
	
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
			//FIXME frigg::infoLogger() << "    Local APIC id: "
			//FIXME 		<< entry->localApicId << frigg::endLog;

//	TODO: APs disabled for now
//			if(seen_bsp)
//				helControlKernel(kThorSubArch, kThorIfBootSecondary,
//						&entry->localApicId, nullptr);
			seen_bsp = 1;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			//FIXME frigg::infoLogger() << "    I/O APIC id: " << entry->ioApicId
			//FIXME 		<< ", sytem interrupt base: " << entry->systemIntBase
			//FIXME 		<< frigg::endLog;
			
			uint64_t address = entry->mmioAddress;
			helControlKernel(kThorSubArch, kThorIfSetupIoApic, &address, nullptr);
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;
			//FIXME frigg::infoLogger() << "    Int override: bus " << entry->bus
			//FIXME 		<< ", irq " << entry->sourceIrq << " -> " << entry->systemInt
			//FIXME 		<< frigg::endLog;
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			//FIXME frigg::infoLogger() << "    Local APIC NMI: processor " << entry->processorId
			//FIXME 		<< ", lint: " << entry->localInt << frigg::endLog;
		}else{
			//FIXME frigg::infoLogger() << "    Unexpected MADT entry of type "
			//FIXME 		<< generic->type << frigg::endLog;
		}
		offset += generic->length;
	}
	helControlKernel(kThorSubArch, kThorIfFinishBoot, nullptr, nullptr);
	
//	dumpNamespace(ACPI_ROOT_OBJECT, 0);

	pciDiscover();

	while(true)
		helix::Dispatcher::global().dispatch();
}

