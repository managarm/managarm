
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/elf.hpp>

#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/linked.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>

#include <frigg/protobuf.hpp>
#include <bragi-naked/ld-server.nakedpb.hpp>

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;
namespace protobuf = frigg::protobuf;

#include "linker.hpp"

uintptr_t libraryBase = 0x41000000;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

SharedObject::SharedObject() : baseAddress(0), loadScope(nullptr),
		dynamic(nullptr), globalOffsetTable(nullptr), entry(nullptr),
		hashTableOffset(0), symbolTableOffset(0), stringTableOffset(0),
		lazyRelocTableOffset(0), lazyTableSize(0),
		lazyExplicitAddend(false) { }

void processCopyRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
	ASSERT(type == R_X86_64_COPY);
	
	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ASSERT(symbol->st_name != 0);

	const char *symbol_str = (const char *)(object->baseAddress
			+ object->stringTableOffset + symbol->st_name);
	uintptr_t copy_addr = (uintptr_t)object->loadScope->resolveSymbol(symbol_str,
			object, Scope::kResolveCopy);
	ASSERT(copy_addr != 0);

	memcpy((void *)rel_addr, (void *)copy_addr, symbol->st_size);
}

void processCopyRelocations(SharedObject *object) {
	bool has_rela_offset = false, has_rela_length = false;
	uintptr_t rela_offset;
	size_t rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			has_rela_offset = true;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			has_rela_length = true;
			break;
		case DT_RELAENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(has_rela_offset && has_rela_length) {
		for(size_t offset = 0; offset < rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + rela_offset + offset);
			processCopyRela(object, reloc);
		}
	}else{
		ASSERT(!has_rela_offset && !has_rela_length);
	}
}

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

Scope::Scope() : objects(*allocator) { }

bool strEquals(const char *str1, const char *str2) {
	while(*str1 != 0 && *str2 != 0) {
		if(*str1++ != *str2++)
			return false;
	}
	if(*str1 != 0 || *str2 != 0)
		return false;
	return true;
}

// TODO: move this to some namespace or class?
void *resolveInObject(SharedObject *object, const char *resolve_str) {
	auto hash_table = (Elf64_Word *)(object->baseAddress + object->hashTableOffset);
	
	Elf64_Word num_buckets = hash_table[0];
	Elf64_Word num_chains = hash_table[1];

	for(size_t i = 0; i < num_chains; i++) {
		auto *symbol = (Elf64_Sym *)(object->baseAddress
				+ object->symbolTableOffset + i * sizeof(Elf64_Sym));
		uint8_t type = symbol->st_info & 0x0F;
		uint8_t bind = symbol->st_info >> 4;
		if(bind != STB_GLOBAL)
			continue; // TODO: support local and weak symbols
		if(symbol->st_shndx == SHN_UNDEF)
			continue;
		ASSERT(symbol->st_name != 0);

		const char *symbol_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + symbol->st_name);
		//FIXME infoLogger->log() << (void *)object->baseAddress
		//		<< " " << (void *)object->stringTableOffset
		//		<< " " << (void *)symbol->st_name << " " << (void *)symbol_str << debug::Finish();
		if(strEquals(symbol_str, resolve_str))
			return (void *)(object->baseAddress + symbol->st_value);
	}
	
	return nullptr;
}

// TODO: let this return uintptr_t
void *Scope::resolveSymbol(const char *resolve_str,
		SharedObject *from_object, uint32_t flags) {
	for(size_t i = 0; i < objects.size(); i++) {
		if((flags & kResolveCopy) != 0 && objects[i] == from_object)
			continue;

		void *resolved = resolveInObject(objects[i], resolve_str);
		if(resolved != nullptr)
			return resolved;
	}

	return nullptr;
}


// --------------------------------------------------------
// Loader
// --------------------------------------------------------

Loader::Loader(Scope *scope)
: p_scope(scope), p_processQueue(*allocator) { }

template<typename Reader>
void processSegment(SharedObject *object, Reader reader, int segment_index) {
	uintptr_t virt_address;
	size_t virt_length;
	uint32_t access;

	while(!reader.atEnd()) {
		auto header = protobuf::fetchHeader(reader);
		switch(header.field) {
		case managarm::ld_server::Segment::kField_virt_address:
			virt_address = protobuf::fetchUInt64(reader);
			break;
		case managarm::ld_server::Segment::kField_virt_length:
			virt_length = protobuf::fetchUInt64(reader);
			break;
		case managarm::ld_server::Segment::kField_access:
			access = protobuf::fetchInt32(reader);
			break;
		default:
			ASSERT(!"Unexpected field in managarm.ld_server.Segment message");
		}
	}

	uint32_t map_flags = 0;
	if(access == managarm::ld_server::Access::READ_WRITE) {
		map_flags |= kHelMapReadWrite;
	}else{
		ASSERT(access == managarm::ld_server::Access::READ_EXECUTE);
		map_flags |= kHelMapReadExecute;
	}
	
	int64_t async_id;
	helSubmitRecvDescriptor(serverPipe->getHandle(), eventHub->getHandle(),
			1, 1 + segment_index, kHelNoFunction, kHelNoObject, &async_id);
	HelHandle memory = eventHub->waitForRecvDescriptor(async_id);

	void *actual_pointer;
	helMapMemory(memory, kHelNullHandle, (void *)virt_address, virt_length,
			map_flags, &actual_pointer);
}

template<typename Reader>
void processServerResponse(SharedObject *object, Reader reader) {
	int segment_index = 0;

	while(!reader.atEnd()) {
		auto header = protobuf::fetchHeader(reader);
		switch(header.field) {
		case managarm::ld_server::ServerResponse::kField_entry:
			object->entry = (void *)protobuf::fetchUInt64(reader);
			break;
		case managarm::ld_server::ServerResponse::kField_dynamic:
			object->dynamic = (Elf64_Dyn *)protobuf::fetchUInt64(reader);
			break;
		case managarm::ld_server::ServerResponse::kField_segments:
			processSegment(object, protobuf::fetchMessage(reader),
					segment_index++);
			break;
		default:
			ASSERT(!"Unexpected field in ServerResponse");
		}
	}
}

void Loader::load(SharedObject *object, const char *file) {
	infoLogger->log() << "Loading " << file << debug::Finish();

	protobuf::FixedWriter<64> writer;
	protobuf::emitCString(writer,
			managarm::ld_server::ClientRequest::kField_identifier, file);
	protobuf::emitUInt64(writer,
			managarm::ld_server::ClientRequest::kField_base_address,
			object->baseAddress);
	serverPipe->sendString(writer.data(), writer.size(), 1, 0);
	
	uint8_t buffer[128];
	int64_t async_id;
	helSubmitRecvString(serverPipe->getHandle(), eventHub->getHandle(),
			buffer, 128, 1, 0, kHelNoFunction, kHelNoObject, &async_id);
	size_t length = eventHub->waitForRecvString(async_id);
	processServerResponse(object, protobuf::BufferReader(buffer, length));
	
	parseDynamic(object);
	p_processQueue.addBack(object);
	p_scope->objects.push(object);
}

void Loader::process() {
	while(!p_processQueue.empty()) {
		SharedObject *object = p_processQueue.front();
		object->loadScope = p_scope;
		infoLogger->log() << "process at " << (void *)object->baseAddress << debug::Finish();

		processDependencies(object);
		processStaticRelocations(object);
		processLazyRelocations(object);

		p_processQueue.removeFront();
	}
}

void Loader::parseDynamic(SharedObject *object) {
	ASSERT(object->dynamic != nullptr);

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		switch(dynamic->d_tag) {
		// handle hash table, symbol table and string table
		case DT_HASH:
			object->hashTableOffset = dynamic->d_ptr;
			break;
		case DT_STRTAB:
			object->stringTableOffset = dynamic->d_ptr;
			break;
		case DT_STRSZ:
			break; // we don't need the size of the string table
		case DT_SYMTAB:
			object->symbolTableOffset = dynamic->d_ptr;
			break;
		case DT_SYMENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Sym));
			break;
		// handle lazy relocation table
		case DT_PLTGOT:
			object->globalOffsetTable = (void **)(object->baseAddress
					+ dynamic->d_ptr);
			break;
		case DT_JMPREL:
			object->lazyRelocTableOffset = dynamic->d_ptr;
			break;
		case DT_PLTRELSZ:
			object->lazyTableSize = dynamic->d_val;
			break;
		case DT_PLTREL:
			if(dynamic->d_val == DT_RELA) {
				object->lazyExplicitAddend = true;
			}else{
				ASSERT(dynamic->d_val == DT_REL);
			}
			break;
		// ignore unimportant tags
		case DT_SONAME: case DT_NEEDED: // we handle this later
		case DT_INIT: case DT_FINI:
		case DT_DEBUG:
		case DT_RELA: case DT_RELASZ: case DT_RELAENT: case DT_RELACOUNT:
		case DT_VERSYM:
		case DT_VERDEF: case DT_VERDEFNUM:
		case DT_VERNEED: case DT_VERNEEDNUM:
			break;
		default:
			debug::panicLogger.log() << "Unexpected dynamic entry "
					<< (void *)dynamic->d_tag << " in object" << debug::Finish();
		}
	}
}

void Loader::processDependencies(SharedObject *object) {
	// load required dynamic libraries
	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		if(dynamic->d_tag != DT_NEEDED)
			continue;

		const char *library_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + dynamic->d_val);

		auto library = memory::construct<SharedObject>(*allocator);
		library->baseAddress = libraryBase;
		// TODO: handle this dynamically
		libraryBase += 0x1000000; // assume 16 MiB per library
		load(library, library_str);
	}
}

void Loader::processRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
	
	if(type == R_X86_64_COPY)
		return; // TODO: make sure this only occurs in executables

	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	// resolve the symbol if there is a symbol
	uintptr_t symbol_addr = 0;
	if(symbol_index != 0) {
		auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
				+ symbol_index * sizeof(Elf64_Sym));
		ASSERT(symbol->st_name != 0);

		const char *symbol_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + symbol->st_name);
		//FIXME infoLogger->log() << "Looking up " << symbol_str << debug::Finish();
		symbol_addr = (uintptr_t)object->loadScope->resolveSymbol(symbol_str, object, 0);
		if(symbol_addr == 0 && ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
			debug::panicLogger.log() << "Unresolved static symbol "
					<< (const char *)symbol_str << debug::Finish();
	}

	switch(type) {
	case R_X86_64_64:
		*((uint64_t *)rel_addr) = symbol_addr + reloc->r_addend;
		//FIXME infoLogger->log() << "R_X86_64_64 at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(symbol_addr + reloc->r_addend)
		//		<< debug::Finish();
		break;
	case R_X86_64_GLOB_DAT:
		*((uint64_t *)rel_addr) = symbol_addr;
		//FIXME infoLogger->log() << "R_X86_64_GLOB_DAT at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(symbol_addr)
		//		<< debug::Finish();
		break;
	case R_X86_64_RELATIVE:
		*((uint64_t *)rel_addr) = object->baseAddress + reloc->r_addend;
		//FIXME infoLogger->log() << "R_X86_64_RELATIVE at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(object->baseAddress + reloc->r_addend)
		//		<< debug::Finish();
		break;
	default:
		debug::panicLogger.log() << "Unexpected relocation type "
				<< (void *)type << debug::Finish();
	}
}

void Loader::processStaticRelocations(SharedObject *object) {
	bool has_rela_offset = false, has_rela_length = false;
	uintptr_t rela_offset;
	size_t rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			has_rela_offset = true;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			has_rela_length = true;
			break;
		case DT_RELAENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(has_rela_offset && has_rela_length) {
		for(size_t offset = 0; offset < rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + rela_offset + offset);
			processRela(object, reloc);
		}
	}else{
		ASSERT(!has_rela_offset && !has_rela_length);
	}
}

void Loader::processLazyRelocations(SharedObject *object) {
	if(object->globalOffsetTable == nullptr) {
		ASSERT(object->lazyRelocTableOffset == 0);
		return;
	}

	object->globalOffsetTable[1] = object;
	object->globalOffsetTable[2] = (void *)&pltRelocateStub;
	
	// adjust the addresses of JUMP_SLOT relocations
	ASSERT(object->lazyExplicitAddend);
	for(size_t offset = 0; offset < object->lazyTableSize; offset += sizeof(Elf64_Rela)) {
		auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset + offset);
		Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
		Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

		uintptr_t rel_addr = object->baseAddress + reloc->r_offset;

		ASSERT(type == R_X86_64_JUMP_SLOT);
		*((uint64_t *)rel_addr) += object->baseAddress;
	}
}

