
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <iostream>
#include <sstream>
#include <vector>

#include <helix/ipc.hpp>
#include <thor.h>

extern "C" {
#include <acpi.h>
}

#include "common.hpp"

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
	ACPI_GENERIC_ADDRESS address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

void acpicaCheckFailed(const char *expr, const char *file, int line) {
	std::cout << "ACPICA_CHECK failed: "
			<< expr << "\nIn file " << file << " on line " << line
			<< std::endl;
}

#define ACPICA_CHECK(expr) do { if((expr) != AE_OK) { \
		acpicaCheckFailed(#expr, __FILE__, __LINE__); } } while(0)

namespace acpi {
	struct ScopedBuffer {
		ScopedBuffer() {
			_object.Length = ACPI_ALLOCATE_BUFFER;
			_object.Pointer = nullptr;
		}

		ScopedBuffer(const ScopedBuffer &) = delete;

		~ScopedBuffer() {
			if(_object.Pointer)
				AcpiOsFree(_object.Pointer);
		}

		ScopedBuffer &operator= (const ScopedBuffer &) = delete;

		size_t size() {
			assert(_object.Pointer);
			return _object.Length;
		}

		void *data() {
			assert(_object.Pointer);
			return _object.Pointer;
		}

		ACPI_BUFFER *get() {
			return &_object;
		}

	private:
		ACPI_BUFFER _object;
	};
};

bool hasChildren(ACPI_HANDLE parent) {
	ACPI_HANDLE child;
	ACPI_STATUS status = AcpiGetNextObject(ACPI_TYPE_ANY, parent, nullptr, &child);
	if(status == AE_NOT_FOUND)
		return false;
	ACPICA_CHECK(status);
	return true;
}

bool hasChild(ACPI_HANDLE parent, const char *path) {
	ACPI_HANDLE child = nullptr;
	while(true) {
		ACPI_STATUS status = AcpiGetNextObject(ACPI_TYPE_ANY, parent, child, &child);
		if(status == AE_NOT_FOUND)
			return false;
		ACPICA_CHECK(status);
	
		acpi::ScopedBuffer buffer;
		ACPICA_CHECK(AcpiGetName(child, ACPI_SINGLE_NAME, buffer.get()));
		if(!strcmp(static_cast<char *>(buffer.data()), path))
			return true;
	}
}

std::vector<ACPI_HANDLE> getChildren(ACPI_HANDLE parent) {
	std::vector<ACPI_HANDLE> results;
	ACPI_HANDLE child = nullptr;
	while(true) {
		ACPI_STATUS status = AcpiGetNextObject(ACPI_TYPE_ANY, parent, child, &child);
		if(status == AE_NOT_FOUND)
			break;
		ACPICA_CHECK(status);
		
		results.push_back(child);
	}
	return results;
}

template<typename F>
void walkResources(ACPI_HANDLE object, const char *method, F functor) {
	auto fptr = [] (ACPI_RESOURCE *r, void *c) -> ACPI_STATUS {
		(*static_cast<F *>(c))(r);
		return AE_OK;
	};
	ACPICA_CHECK(AcpiWalkResources(object, const_cast<char *>(method), fptr, &functor));
}

void dumpNamespace(ACPI_HANDLE object, int depth = 0) {
	ACPI_OBJECT_TYPE type;
	ACPICA_CHECK(AcpiGetType(object, &type));

	char segment[5];
	ACPI_BUFFER name_buffer;
	name_buffer.Pointer = segment;
	name_buffer.Length = 5;
	ACPICA_CHECK(AcpiGetName(object, ACPI_SINGLE_NAME, &name_buffer));

	auto indent = [&] {
		for(int i = 0; i < depth; i++)
			std::cout << "    ";
	};

	auto typeString = [] (ACPI_OBJECT_TYPE type) -> std::string {
		if(type == ACPI_TYPE_INTEGER) {
			return "Integer";
		}else if(type == ACPI_TYPE_STRING) {
			return "String";
		}else if(type == ACPI_TYPE_BUFFER) {
			return "Buffer";
		}else if(type == ACPI_TYPE_PACKAGE) {
			return "Package";
		}else if(type == ACPI_TYPE_DEVICE) {
			return "Device";
		}else if(type == ACPI_TYPE_METHOD) {
			return "Method";
		}else if(type == ACPI_TYPE_MUTEX) {
			return "Mutex";
		}else if(type == ACPI_TYPE_REGION) {
			return "Region";
		}else if(type == ACPI_TYPE_PROCESSOR) {
			return "Processor";
		}else if(type == ACPI_TYPE_LOCAL_SCOPE) {
			return "Scope";
		}else{
			std::stringstream s;
			s << "[Type 0x" << std::hex << type << std::dec << "]";
			return s.str();
		}
	};

	indent();
	std::cout << segment << ": " << typeString(type);

	if(type == ACPI_TYPE_INTEGER) {
		ACPI_OBJECT result;

		ACPI_BUFFER buffer;
		buffer.Pointer = &result;
		buffer.Length = sizeof(ACPI_OBJECT);
		ACPICA_CHECK(AcpiEvaluateObjectTyped(object, nullptr,
				nullptr, &buffer, ACPI_TYPE_INTEGER));

		std::cout << " 0x" << std::hex << result.Integer.Value << std::dec;
	}
	std::cout << std::endl;
	
	if(hasChild(object, "_CRS")) {
		walkResources(object, "_CRS", [&] (ACPI_RESOURCE *r) {
			if(r->Type == ACPI_RESOURCE_TYPE_IRQ) {
				indent();
				std::cout << "* Resource: Irq (";
				for(uint8_t i = 0; i < r->Data.Irq.InterruptCount; i++) {
					if(i)
						std::cout << ", ";
					std::cout << (int)r->Data.Irq.Interrupts[i];
				}
				std::cout << ")" << std::endl;
			}else if(r->Type == ACPI_RESOURCE_TYPE_DMA) {
				indent();
				std::cout << "* Resource: Dma" << std::endl;
			}else if(r->Type == ACPI_RESOURCE_TYPE_IO) {
				indent();
				std::cout << "* Resource: Io ("
						<< std::hex << "Base: 0x" << r->Data.Io.Minimum
						<< ", Length: 0x" << (int)r->Data.Io.AddressLength << std::dec
						<< ")" << std::endl;
			}else if(r->Type == ACPI_RESOURCE_TYPE_ADDRESS16) {
				indent();
				std::cout << "* Resource: Address16 ("
						<< std::hex << "Base: 0x" << r->Data.Address16.Address.Minimum
						<< ", Length: 0x" << r->Data.Address16.Address.AddressLength << std::dec
						<< ")" << std::endl;
			}else if(r->Type == ACPI_RESOURCE_TYPE_ADDRESS32) {
				indent();
				std::cout << "* Resource: Address32 ("
						<< std::hex << "Base: 0x" << r->Data.Address32.Address.Minimum
						<< ", Length: 0x" << r->Data.Address32.Address.AddressLength << std::dec
						<< ")" << std::endl;
			}else if(r->Type != ACPI_RESOURCE_TYPE_END_TAG) {
				indent();
				std::cout << "* Resource: [Type 0x"
						<< std::hex << r->Type << std::dec << "]" << std::endl;
			}
		});
	}
	
/*	if(strcmp(segment, "PCI0") == 0) {
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
	}*/

	auto children = getChildren(object);
	for(ACPI_HANDLE child : children)
		dumpNamespace(child, depth + 1);
}

void dumpNamespace() {
	auto children = getChildren(ACPI_ROOT_OBJECT);
	for(ACPI_HANDLE child : children)
		dumpNamespace(child);
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
	assert(hpet_entry->address.SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY);
	helControlKernel(kThorSubArch, kThorIfSetupHpet, &hpet_entry->address.Address, nullptr);

	// boot secondary processors
	ACPI_TABLE_HEADER *madt_table;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("APIC"), 0, &madt_table));
	
	int seen_bsp = 0;

	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt_table->Length) {
		auto generic = (MadtGenericEntry *)((uintptr_t)madt_table + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			std::cout << "    Local APIC id: "
					<< (int)entry->localApicId << std::endl;

			uint32_t id = entry->localApicId;
			if(seen_bsp)
				helControlKernel(kThorSubArch, kThorIfBootSecondary,
						&id, nullptr);
			seen_bsp = 1;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			std::cout << "    I/O APIC id: " << (int)entry->ioApicId
					<< ", sytem interrupt base: " << (int)entry->systemIntBase
					<< std::endl;
			
			uint64_t address = entry->mmioAddress;
			helControlKernel(kThorSubArch, kThorIfSetupIoApic, &address, nullptr);
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;
			std::cout << "    Int override: bus " << (int)entry->bus
					<< ", irq " << (int)entry->sourceIrq << " -> " << (int)entry->systemInt
					<< std::endl;
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			std::cout << "    Local APIC NMI: processor " << (int)entry->processorId
					<< ", lint: " << (int)entry->localInt << std::endl;
		}else{
			std::cout << "    Unexpected MADT entry of type "
					<< generic->type << std::endl;
		}
		offset += generic->length;
	}
	helControlKernel(kThorSubArch, kThorIfFinishBoot, nullptr, nullptr);
	
	//dumpNamespace();

	pciDiscover();

	while(true)
		helix::Dispatcher::global().dispatch();
}

