
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/elf.hpp>

#include <frigg/tuple.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/linked.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include <frigg/glue-hel.hpp>
#include <frigg/protobuf.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

#include "linker.hpp"

uintptr_t libraryBase = 0x41000000;

bool verbose = false;
bool eagerBinding = true;

// This is the global "resolution timestamp" (RTS) counter.
// It is incremented each time __dlapi_open() (i.e. dlopen()) is called.
// Each DSO stores its objectRts (i.e. RTS at the time the object was loaded).
// DSOs in the global scope also store a globalRts (i.e. RTS at the time the
// object became global). This mechanism is used to determine which
// part of the global scope is considered for symbol resolution.
uint64_t rtsCounter = 2;

// --------------------------------------------------------
// POSIX I/O functions.
// --------------------------------------------------------

template<typename T>
T load(void *ptr) {
	T result;
	memcpy(&result, ptr, sizeof(T));
	return result;
}

struct Queue {
	Queue()
	: _queue(nullptr), _progress(0) { }

	HelQueue *getQueue() {
		if(!_queue) {
			auto ptr = allocator->allocate(sizeof(HelQueue) + 4096);
			_queue = reinterpret_cast<HelQueue *>(ptr);
			_queue->elementLimit = 128;
			_queue->queueLength = 4096;
			_queue->kernelState = 0;
			_queue->userState = 0;
		}
		return _queue;
	}

	void *dequeueSingle() {
		auto ke = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
		while(true) {
			assert(!(ke & kHelQueueWantNext));

			if(_progress < (ke & kHelQueueTail)) {
				auto ptr = (char *)_queue + sizeof(HelQueue) + _progress;
				auto elem = load<HelElement>(ptr);
				_progress += sizeof(HelElement) + elem.length;
				return ptr + sizeof(HelElement);
			}

			if(!(ke & kHelQueueWaiters)) {
				auto d = ke | kHelQueueWaiters;
				if(__atomic_compare_exchange_n(&_queue->kernelState,
						&ke, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
					ke = d;
			}else{
				HEL_CHECK(helFutexWait((int *)&_queue->kernelState, ke));
				ke = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
			}
		}
	}

private:
	HelQueue *_queue;
	size_t _progress;
};

HelSimpleResult *parseSimple(void *&element) {
	auto result = reinterpret_cast<HelSimpleResult *>(element);
	element = (char *)element + sizeof(HelSimpleResult);
	return result;
}

HelInlineResult *parseInline(void *&element) {
	auto result = reinterpret_cast<HelInlineResult *>(element);
	element = (char *)element + sizeof(HelInlineResult)
			+ ((result->length + 7) & ~size_t(7));
	return result;
}

HelLengthResult *parseLength(void *&element) {
	auto result = reinterpret_cast<HelLengthResult *>(element);
	element = (char *)element + sizeof(HelLengthResult);
	return result;
}

HelHandleResult *parseHandle(void *&element) {
	auto result = reinterpret_cast<HelHandleResult *>(element);
	element = (char *)element + sizeof(HelHandleResult);
	return result;
}

int posixOpen(frigg::StringView path) {
	HelAction actions[3];

	managarm::posix::CntRequest<Allocator> req(*allocator);
	req.set_request_type(managarm::posix::CntReqType::OPEN);
	req.set_path(frigg::String<Allocator>(*allocator, path));

	Queue m;

	frigg::String<Allocator> ser(*allocator);
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = 0;
	HEL_CHECK(helSubmitAsync(kHelThisThread, actions, 3, m.getQueue(), 0, 0));

	auto element = m.dequeueSingle();
	auto offer = parseSimple(element);
	auto send_req = parseSimple(element);
	auto recv_resp = parseInline(element);
	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	
	managarm::posix::SvrResponse<Allocator> resp(*allocator);
	resp.ParseFromArray(recv_resp->data, recv_resp->length);

	if(resp.error() == managarm::posix::Errors::FILE_NOT_FOUND)
		return -1;
	assert(resp.error() == managarm::posix::Errors::SUCCESS);
	return resp.fd();
}

void posixSeek(int fd, int64_t offset) {
	auto lane = fileTable[fd];

	HelAction actions[3];

	managarm::fs::CntRequest<Allocator> req(*allocator);
	req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	req.set_rel_offset(offset);
	
	Queue m;

	frigg::String<Allocator> ser(*allocator);
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = 0;
	HEL_CHECK(helSubmitAsync(lane, actions, 3, m.getQueue(), 0, 0));

	auto element = m.dequeueSingle();
	auto offer = parseSimple(element);
	auto send_req = parseSimple(element);
	auto recv_resp = parseInline(element);
	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	
	managarm::fs::SvrResponse<Allocator> resp(*allocator);
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
}

void posixRead(int fd, void *data, size_t length) {
	auto lane = fileTable[fd];

	size_t offset = 0;
	while(offset < length) {
		HelAction actions[4];

		managarm::fs::CntRequest<Allocator> req(*allocator);
		req.set_req_type(managarm::fs::CntReqType::READ);
		req.set_size(length - offset);
	
		Queue m;

		frigg::String<Allocator> ser(*allocator);
		req.SerializeToString(&ser);
		actions[0].type = kHelActionOffer;
		actions[0].flags = kHelItemAncillary;
		actions[1].type = kHelActionSendFromBuffer;
		actions[1].flags = kHelItemChain;
		actions[1].buffer = ser.data();
		actions[1].length = ser.size();
		actions[2].type = kHelActionRecvInline;
		actions[2].flags = kHelItemChain;
		actions[3].type = kHelActionRecvToBuffer;
		actions[3].flags = 0;
		actions[3].buffer = (char *)data + offset;
		actions[3].length = length - offset;
		HEL_CHECK(helSubmitAsync(lane, actions, 4, m.getQueue(), 0, 0));

		auto element = m.dequeueSingle();
		auto offer = parseSimple(element);
		auto send_req = parseSimple(element);
		auto recv_resp = parseInline(element);
		auto recv_data = parseLength(element);
		HEL_CHECK(offer->error);
		HEL_CHECK(send_req->error);
		HEL_CHECK(recv_resp->error);
		HEL_CHECK(recv_data->error);

		managarm::fs::SvrResponse<Allocator> resp(*allocator);
		resp.ParseFromArray(recv_resp->data, recv_resp->length);
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		offset += recv_data->length;
	}
	assert(offset == length);
}

HelHandle posixMmap(int fd) {
	auto lane = fileTable[fd];

	HelAction actions[4];

	managarm::fs::CntRequest<Allocator> req(*allocator);
	req.set_req_type(managarm::fs::CntReqType::MMAP);

	Queue m;

	frigg::String<Allocator> ser(*allocator);
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = kHelItemChain;
	actions[3].type = kHelActionPullDescriptor;
	actions[3].flags = 0;
	HEL_CHECK(helSubmitAsync(lane, actions, 4, m.getQueue(), 0, 0));

	auto element = m.dequeueSingle();
	auto offer = parseSimple(element);
	auto send_req = parseSimple(element);
	auto recv_resp = parseInline(element);
	auto pull_memory = parseHandle(element);
	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	HEL_CHECK(pull_memory->error);
	
	managarm::fs::SvrResponse<Allocator> resp(*allocator);
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	return pull_memory->handle;
}

void posixClose(int fd) {
	HelAction actions[3];

	managarm::posix::CntRequest<Allocator> req(*allocator);
	req.set_request_type(managarm::posix::CntReqType::CLOSE);
	req.set_fd(fd);
	
	Queue m;

	frigg::String<Allocator> ser(*allocator);
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = 0;
	HEL_CHECK(helSubmitAsync(kHelThisThread, actions, 3, m.getQueue(), 0, 0));

	auto element = m.dequeueSingle();
	auto offer = parseSimple(element);
	auto send_req = parseSimple(element);
	auto recv_resp = parseInline(element);
	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	
	managarm::posix::SvrResponse<Allocator> resp(*allocator);
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	assert(resp.error() == managarm::posix::Errors::SUCCESS);
}

// --------------------------------------------------------
// ObjectRepository
// --------------------------------------------------------

ObjectRepository::ObjectRepository()
: _nameMap{frigg::DefaultHasher<frigg::StringView>{}, *allocator} { }

SharedObject *ObjectRepository::injectObjectFromDts(frigg::StringView name,
		uintptr_t base_address, Elf64_Dyn *dynamic, uint64_t rts) {
	assert(!_nameMap.get(name));

	auto object = frigg::construct<SharedObject>(*allocator, name.data(), false, rts);
	object->baseAddress = base_address;
	object->dynamic = dynamic;
	_parseDynamic(object);

	_nameMap.insert(name, object);
	_discoverDependencies(object, rts);

	return object;
}

SharedObject *ObjectRepository::injectObjectFromPhdrs(frigg::StringView name,
		void *phdr_pointer, size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer,
		uint64_t rts) {
	assert(!_nameMap.get(name));

	auto object = frigg::construct<SharedObject>(*allocator, name.data(), true, rts);
	_fetchFromPhdrs(object, phdr_pointer, phdr_entry_size, num_phdrs, entry_pointer);
	_parseDynamic(object);

	_nameMap.insert(name, object);
	_discoverDependencies(object, rts);

	return object;
}

SharedObject *ObjectRepository::requestObjectWithName(frigg::StringView name, uint64_t rts) {
	auto it = _nameMap.get(name);
	if(it)
		return *it;

	auto object = frigg::construct<SharedObject>(*allocator, name.data(), false, rts);

	frigg::String<Allocator> lib_prefix(*allocator, "/lib/");
	frigg::String<Allocator> usr_prefix(*allocator, "/usr/lib/");

	// open the object file
	auto fd = posixOpen(lib_prefix + name);
	if(fd == -1)
		fd = posixOpen(usr_prefix + name);
	if(fd == -1)
		return nullptr; // TODO: Free the SharedObject.
	_fetchFromFile(object, fd);
	posixClose(fd);

	_parseDynamic(object);

	_nameMap.insert(name, object);
	_discoverDependencies(object, rts);

	return object;
}

SharedObject *ObjectRepository::requestObjectAtPath(frigg::StringView path, uint64_t rts) {
	// TODO: Support SONAME correctly.
	auto it = _nameMap.get(path);
	if(it)
		return *it;

	auto object = frigg::construct<SharedObject>(*allocator, path.data(), false, rts);
	
	auto fd = posixOpen(path);
	if(fd == -1)
		return nullptr; // TODO: Free the SharedObject.
	_fetchFromFile(object, fd);
	posixClose(fd);

	_parseDynamic(object);

	_nameMap.insert(path, object);
	_discoverDependencies(object, rts);

	return object;
}

// --------------------------------------------------------
// ObjectRepository: Fetching methods.
// --------------------------------------------------------

void ObjectRepository::_fetchFromPhdrs(SharedObject *object, void *phdr_pointer,
		size_t phdr_entry_size, size_t phdr_count, void *entry_pointer) {
	assert(object->isMainObject);
	if(verbose)
		frigg::infoLogger() << "rtdl: Loading " << object->name << frigg::endLog;
	
	object->entry = entry_pointer;

	// segments are already mapped, so we just have to find the dynamic section
	for(size_t i = 0; i < phdr_count; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)phdr_pointer + i * phdr_entry_size);
		switch(phdr->p_type) {
		case PT_DYNAMIC:
			object->dynamic = (Elf64_Dyn *)(object->baseAddress + phdr->p_vaddr);
			break;
		case PT_TLS: {
			object->tlsSegmentSize = phdr->p_memsz;
			object->tlsAlignment = phdr->p_align;
			object->tlsImageSize = phdr->p_filesz;
			object->tlsImagePtr = (void *)(object->baseAddress + phdr->p_vaddr);
		} break;
		default:
			//FIXME warn about unknown phdrs
			break;
		}
	}
}


void ObjectRepository::_fetchFromFile(SharedObject *object, int fd) {
	assert(!object->isMainObject);

	object->baseAddress = libraryBase;
	// TODO: handle this dynamically
	libraryBase += 0x1000000; // assume 16 MiB per library

	if(verbose)
		frigg::infoLogger() << "rtdl: Loading " << object->name
				<< " at " << (void *)object->baseAddress << frigg::endLog;

	// read the elf file header
	Elf64_Ehdr ehdr;
	posixRead(fd, &ehdr, sizeof(Elf64_Ehdr));

	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');
	assert(ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

	// read the elf program headers
	auto phdr_buffer = (char *)allocator->allocate(ehdr.e_phnum * ehdr.e_phentsize);
	posixSeek(fd, ehdr.e_phoff);
	posixRead(fd, phdr_buffer, ehdr.e_phnum * ehdr.e_phentsize);

	// mmap the file so we can map read-only segments instead of copying them
	HelHandle file_memory = posixMmap(fd);

	constexpr size_t kPageSize = 0x1000;
	
	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdr_buffer + i * ehdr.e_phentsize);
		
		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);
			
			assert(object->baseAddress % kPageSize == 0);
			size_t misalign = phdr->p_vaddr % kPageSize;

			uintptr_t map_address = object->baseAddress + phdr->p_vaddr - misalign;
			size_t map_length = phdr->p_memsz + misalign;
			if((map_length % kPageSize) != 0)
				map_length += kPageSize - (map_length % kPageSize);
			
			if(!(phdr->p_flags & PF_W)) {
				assert((phdr->p_offset % kPageSize) == 0);

				// map the segment with correct permissions
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(file_memory, phdr->p_offset, map_length));

					void *map_pointer;
					HEL_CHECK(helMapMemory(file_memory, kHelNullHandle,
							(void *)map_address, phdr->p_offset, map_length,
							kHelMapProtRead | kHelMapProtExecute | kHelMapShareAtFork,
							&map_pointer));
				}else{
					frigg::panicLogger() << "Illegal combination of segment permissions"
							<< frigg::endLog;
				}
			}else{
				// setup the segment with write permission and copy data
				HelHandle memory;
				HEL_CHECK(helAllocateMemory(map_length, 0, &memory));

				void *write_ptr;
				HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
						0, map_length, kHelMapProtRead | kHelMapProtWrite | kHelMapDropAtFork,
						&write_ptr));

				memset(write_ptr, 0, map_length);
				posixSeek(fd, phdr->p_offset);
				posixRead(fd, (char *)write_ptr + misalign, phdr->p_filesz);
				HEL_CHECK(helUnmapMemory(kHelNullHandle, write_ptr, map_length));

				// map the segment with correct permissions
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
					void *map_pointer;
					HEL_CHECK(helMapMemory(memory, kHelNullHandle, (void *)map_address,
							0, map_length, kHelMapProtRead | kHelMapProtWrite
								| kHelMapCopyOnWriteAtFork,
							&map_pointer));
				}else{
					frigg::panicLogger() << "Illegal combination of segment permissions"
							<< frigg::endLog;
				}
			}
		}else if(phdr->p_type == PT_TLS) {
			object->tlsSegmentSize = phdr->p_memsz;
			object->tlsAlignment = phdr->p_align;
			object->tlsImageSize = phdr->p_filesz;
			object->tlsImagePtr = (void *)(object->baseAddress + phdr->p_vaddr);
		}else if(phdr->p_type == PT_DYNAMIC) {
			object->dynamic = (Elf64_Dyn *)(object->baseAddress + phdr->p_vaddr);
		}else if(phdr->p_type == PT_INTERP
				|| phdr->p_type == PT_PHDR
				|| phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_RELRO
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}

	HEL_CHECK(helCloseDescriptor(file_memory));
}

// --------------------------------------------------------
// ObjectRepository: Parsing methods.
// --------------------------------------------------------

void ObjectRepository::_parseDynamic(SharedObject *object) {
	assert(object->dynamic != nullptr);

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
			assert(dynamic->d_val == sizeof(Elf64_Sym));
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
				assert(dynamic->d_val == DT_REL);
			}
			break;
		// TODO: Implement this correctly!
		case DT_SYMBOLIC:
			object->symbolicResolution = true;
			break;
		case DT_BIND_NOW:
			object->eagerBinding = true;
			break;
		case DT_FLAGS:
			if(dynamic->d_val & DF_SYMBOLIC)
				object->symbolicResolution = true;
			if(dynamic->d_val & DF_STATIC_TLS)
				object->haveStaticTls = true;
			if(dynamic->d_val & ~(DF_SYMBOLIC | DF_STATIC_TLS))
				frigg::infoLogger() << "\e[31mrtdl: DT_FLAGS(" << frigg::logHex(dynamic->d_val)
						<< ") is not implemented correctly!\e[39m"
						<< frigg::endLog;
			break;
		case DT_FLAGS_1:
			if(dynamic->d_val & DF_1_NOW)
				object->eagerBinding = true;
			if(dynamic->d_val & ~(DF_1_NOW))
				frigg::infoLogger() << "\e[31mrtdl: DT_FLAGS_1(" << frigg::logHex(dynamic->d_val)
						<< ") is not implemented correctly!\e[39m"
						<< frigg::endLog;
			break;
		// ignore unimportant tags
		case DT_SONAME: case DT_NEEDED: case DT_RPATH: // we handle this later
		case DT_INIT: case DT_FINI:
		case DT_INIT_ARRAY: case DT_INIT_ARRAYSZ:
		case DT_FINI_ARRAY: case DT_FINI_ARRAYSZ:
		case DT_DEBUG:
		case DT_RELA: case DT_RELASZ: case DT_RELAENT: case DT_RELACOUNT:
		case DT_VERSYM:
		case DT_VERDEF: case DT_VERDEFNUM:
		case DT_VERNEED: case DT_VERNEEDNUM:
			break;
		default:
			frigg::panicLogger() << "Unexpected dynamic entry "
					<< (void *)dynamic->d_tag << " in object" << frigg::endLog;
		}
	}
}

void ObjectRepository::_discoverDependencies(SharedObject *object, uint64_t rts) {
	// Load required dynamic libraries.
	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		if(dynamic->d_tag != DT_NEEDED)
			continue;

		const char *library_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + dynamic->d_val);

		auto library = requestObjectWithName(frigg::StringView{library_str}, rts);
		if(!library)
			frigg::panicLogger() << "Could not satisfy dependency " << library_str << frigg::endLog;
		object->dependencies.push(library);
	}
}

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

SharedObject::SharedObject(const char *name, bool is_main_object, uint64_t object_rts)
		: name(name), isMainObject(is_main_object), objectRts(object_rts),
		baseAddress(0), loadScope(nullptr),
		dynamic(nullptr), globalOffsetTable(nullptr), entry(nullptr),
		tlsSegmentSize(0), tlsAlignment(0), tlsImageSize(0), tlsImagePtr(nullptr),
		tlsInitialized(false),
		hashTableOffset(0), symbolTableOffset(0), stringTableOffset(0),
		lazyRelocTableOffset(0), lazyTableSize(0), lazyExplicitAddend(false),
		symbolicResolution(false), eagerBinding(false), haveStaticTls(false),
		dependencies(*allocator),
		tlsModel(TlsModel::null), tlsOffset(0),
		globalRts(0), wasLinked(false),
		scheduledForInit(false), onInitStack(false), wasInitialized(false),
		objectScope(nullptr) { }

void processCopyRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
	if(type != R_X86_64_COPY)
		return;
	
	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ObjectSymbol r(object, symbol);
	frigg::Optional<ObjectSymbol> p = object->loadScope->resolveSymbol(r, Scope::resolveCopy);
	assert(p);

	memcpy((void *)rel_addr, (void *)p->virtualAddress(), symbol->st_size);
}

void processCopyRelocations(SharedObject *object) {
	frigg::Optional<uintptr_t> rela_offset;
	frigg::Optional<size_t> rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			break;
		case DT_RELAENT:
			assert(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(rela_offset && rela_length) {
		for(size_t offset = 0; offset < *rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + *rela_offset + offset);
			processCopyRela(object, reloc);
		}
	}else{
		assert(!rela_offset && !rela_length);
	}
}

void doInitialize(SharedObject *object) {
	assert(object->wasLinked);
	assert(!object->wasInitialized);

	// if the object has dependencies we initialize them first
	for(size_t i = 0; i < object->dependencies.size(); i++)
		assert(object->dependencies[i]->wasInitialized);

	if(verbose)
		frigg::infoLogger() << "rtdl: Initialize " << object->name << frigg::endLog;
	
	// now initialize the actual object
	typedef void (*InitFuncPtr) ();

	InitFuncPtr init_ptr = nullptr;
	InitFuncPtr *init_array = nullptr;
	size_t array_size = 0;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_INIT:
			if(dynamic->d_ptr != 0)
				init_ptr = (InitFuncPtr)(object->baseAddress + dynamic->d_ptr);
			break;
		case DT_INIT_ARRAY:
			if(dynamic->d_ptr != 0)
				init_array = (InitFuncPtr *)(object->baseAddress + dynamic->d_ptr);
			break;
		case DT_INIT_ARRAYSZ:
			array_size = dynamic->d_val;
			break;
		}
	}

	if(verbose)
		frigg::infoLogger() << "rtdl: Running DT_INIT function" << frigg::endLog;
	if(init_ptr != nullptr)
		init_ptr();
	
	if(verbose)
		frigg::infoLogger() << "rtdl: Running DT_INIT_ARRAY functions" << frigg::endLog;
	assert((array_size % sizeof(InitFuncPtr)) == 0);
	for(size_t i = 0; i < array_size / sizeof(InitFuncPtr); i++)
		init_array[i]();

	if(verbose)
		frigg::infoLogger() << "rtdl: Object initialization complete" << frigg::endLog;
	object->wasInitialized = true;
}

// --------------------------------------------------------
// RuntimeTlsMap
// --------------------------------------------------------

RuntimeTlsMap::RuntimeTlsMap()
: initialPtr(0), initialLimit(0) { }

struct Tcb {
	Tcb *selfPointer;
};

void allocateTcb() {
	size_t fs_size = runtimeTlsMap->initialLimit + sizeof(Tcb);
	char *fs_buffer = (char *)allocator->allocate(fs_size);
	memset(fs_buffer, 0, fs_size);

	auto tcb_ptr = (Tcb *)(fs_buffer + runtimeTlsMap->initialLimit);
	tcb_ptr->selfPointer = tcb_ptr;
	HEL_CHECK(helWriteFsBase(tcb_ptr));
}

// --------------------------------------------------------
// ObjectSymbol
// --------------------------------------------------------

ObjectSymbol::ObjectSymbol(SharedObject *object, const Elf64_Sym *symbol)
: _object(object), _symbol(symbol) { }

const char *ObjectSymbol::getString() {
	assert(_symbol->st_name != 0);
	return (const char *)(_object->baseAddress
			+ _object->stringTableOffset + _symbol->st_name);
}

uintptr_t ObjectSymbol::virtualAddress() {
	auto bind = ELF64_ST_BIND(_symbol->st_info);
	assert(bind == STB_GLOBAL || bind == STB_WEAK);
	assert(_symbol->st_shndx != SHN_UNDEF);
	return _object->baseAddress + _symbol->st_value;
}

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

uint32_t elf64Hash(frigg::StringView string) {
	uint32_t h = 0, g;

	for(size_t i = 0; i < string.size(); ++i) {
		h = (h << 4) + (uint32_t)string[i];
		g = h & 0xF0000000;
		if(g)
			h ^= g >> 24;
		h &= 0x0FFFFFFF;
	}

	return h;
}

// TODO: move this to some namespace or class?
frigg::Optional<ObjectSymbol> resolveInObject(SharedObject *object, frigg::StringView string) {
	// Checks if the symbol can be used to satisfy the dependency.
	auto eligible = [&] (ObjectSymbol cand) {
		if(cand.symbol()->st_shndx == SHN_UNDEF)
			return false;

		auto bind = ELF64_ST_BIND(cand.symbol()->st_info);
		if(bind != STB_GLOBAL && bind != STB_WEAK)
			return false;

		return true;
	};

	auto hash_table = (Elf64_Word *)(object->baseAddress + object->hashTableOffset);
	Elf64_Word num_buckets = hash_table[0];
	auto bucket = elf64Hash(string) % num_buckets;

	auto index = hash_table[2 + bucket];
	while(index != 0) {
		ObjectSymbol cand{object, (Elf64_Sym *)(object->baseAddress
				+ object->symbolTableOffset + index * sizeof(Elf64_Sym))};
		if(eligible(cand) && frigg::StringView{cand.getString()} == string)
			return cand;

		index = hash_table[2 + num_buckets + index];
	}

	return frigg::Optional<ObjectSymbol>();
}


frigg::Optional<ObjectSymbol> Scope::resolveWholeScope(Scope *scope,
		frigg::StringView string, ResolveFlags flags) {
	for(size_t i = 0; i < scope->_objects.size(); i++) {
		if((flags & resolveCopy) && scope->_objects[i]->isMainObject)
			continue;

		frigg::Optional<ObjectSymbol> p = resolveInObject(scope->_objects[i], string);
		if(p)
			return p;
	}

	return frigg::Optional<ObjectSymbol>();
}

Scope::Scope()
: _objects(*allocator) { }

void Scope::appendObject(SharedObject *object) {
	_objects.push(object);
}

// TODO: let this return uintptr_t
frigg::Optional<ObjectSymbol> Scope::resolveSymbol(ObjectSymbol r, ResolveFlags flags) {
	for(size_t i = 0; i < _objects.size(); i++) {
		if((flags & resolveCopy) && _objects[i]->isMainObject)
			continue;

		const char *string = (const char *)(r.object()->baseAddress
				+ r.object()->stringTableOffset + r.symbol()->st_name);
		frigg::Optional<ObjectSymbol> p = resolveInObject(_objects[i], string);
		if(p)
			return p;
	}

	return frigg::Optional<ObjectSymbol>();
}


// --------------------------------------------------------
// Loader
// --------------------------------------------------------

Loader::Loader(Scope *scope, bool is_initial_link, uint64_t rts)
: _globalScope(scope), _isInitialLink(is_initial_link), _linkRts(rts),
		_linkSet(frigg::DefaultHasher<SharedObject *>(), *allocator),
		_linkBfs(*allocator),
		_initQueue(*allocator) { }

// TODO: Use an explicit vector to reduce stack usage to O(1)?
void Loader::submitObject(SharedObject *object) {
	if(_linkSet.get(object))
		return;

	_linkSet.insert(object, Token{});
	_linkBfs.addBack(object);

	for(size_t i = 0; i < object->dependencies.size(); i++)
		submitObject(object->dependencies[i]);
}

void Loader::linkObjects() {
	_buildTlsMaps();

	// Promote objects to the global scope.
	for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
		if((*it)->globalRts)
			continue;
		(*it)->globalRts = _linkRts;
		_globalScope->appendObject(*it);
	}

	// Process regular relocations.
	for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
		// Some objects have already been linked before.
		if((*it)->objectRts < _linkRts)
			continue;

		if(verbose)
			frigg::infoLogger() << "rtdl: Linking " << (*it)->name << frigg::endLog;

		assert(!(*it)->wasLinked);
		(*it)->loadScope = _globalScope;

		// TODO: Support this.
		if((*it)->symbolicResolution)
			frigg::infoLogger() << "\e[31mrtdl: DT_SYMBOLIC is not implemented correctly!\e[39m"
					<< frigg::endLog;

		_processStaticRelocations(*it);
		_processLazyRelocations(*it);
	}
	
	// Process copy relocations.
	for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
		if(!(*it)->isMainObject)
			continue;

		// Some objects have already been linked before.
		if((*it)->objectRts < _linkRts)
			continue;

		processCopyRelocations(*it);
	}
	
	for(auto it = _linkBfs.frontIter(); it.okay(); ++it)
		(*it)->wasLinked = true;
}

void Loader::_buildTlsMaps() {
	if(_isInitialLink) {
		assert(runtimeTlsMap->initialPtr == 0);
		assert(runtimeTlsMap->initialLimit == 0);

		assert(!_linkBfs.empty());
		assert(_linkBfs.front()->isMainObject);

		for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
			SharedObject *object = *it;
			assert(object->tlsModel == TlsModel::null);

			if(object->tlsSegmentSize == 0)
				continue;

			assert(16 % object->tlsAlignment == 0);
			runtimeTlsMap->initialPtr += object->tlsSegmentSize;
			size_t misalign = runtimeTlsMap->initialPtr % object->tlsAlignment;
			if(misalign)
				runtimeTlsMap->initialPtr += object->tlsAlignment - misalign;

			object->tlsModel = TlsModel::initial;
			object->tlsOffset = -runtimeTlsMap->initialPtr;
			
			if(verbose)
				frigg::infoLogger() << "rtdl: TLS of " << object->name
						<< " mapped to 0x" << frigg::logHex(object->tlsOffset)
						<< ", size: " << object->tlsSegmentSize
						<< ", alignment: " << object->tlsAlignment << frigg::endLog;
		}

		// Reserve some additional space for future libraries.
		runtimeTlsMap->initialLimit = runtimeTlsMap->initialPtr + 64;
	}else{
		for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
			SharedObject *object = *it;
			
			if(object->tlsModel != TlsModel::null)
				continue;
			if(object->tlsSegmentSize == 0)
				continue;

			// There are some libraries (e.g. Mesa) that require static TLS even though
			// they expect to be dynamically loaded.
			if(object->haveStaticTls) {
				assert(16 % object->tlsAlignment == 0);
				auto ptr = runtimeTlsMap->initialPtr + object->tlsSegmentSize;
				size_t misalign = ptr % object->tlsAlignment;
				if(misalign)
					ptr += object->tlsAlignment - misalign;

				if(ptr > runtimeTlsMap->initialLimit)
					frigg::panicLogger() << "rtdl: Static TLS space exhausted while while"
							" allocating TLS for " << object->name << frigg::endLog;
				runtimeTlsMap->initialPtr = ptr;

				object->tlsModel = TlsModel::initial;
				object->tlsOffset = -runtimeTlsMap->initialPtr;
				
//				if(verbose)
					frigg::infoLogger() << "rtdl: TLS of " << object->name
							<< " mapped to 0x" << frigg::logHex(object->tlsOffset)
							<< ", size: " << object->tlsSegmentSize
							<< ", alignment: " << object->tlsAlignment << frigg::endLog;
			}else{
				// TODO: Implement dynamic TLS.
				frigg::panicLogger() << "rtdl: Dynamic TLS is not supported" << frigg::endLog;
			}
		}
	}
}

void Loader::initObjects() {
	// Initialize TLS segments that follow the static model.
	for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
		SharedObject *object = *it;
		
		if(object->tlsModel == TlsModel::initial) {
			if(object->tlsInitialized)
				continue;

			char *tcb_ptr;
			asm volatile ("mov %%fs:(0), %0" : "=r"(tcb_ptr));
			auto tls_ptr = tcb_ptr + object->tlsOffset;
			memcpy(tls_ptr, object->tlsImagePtr, object->tlsImageSize);

			object->tlsInitialized = true;
		}
	}

	for(auto it = _linkBfs.frontIter(); it.okay(); ++it) {
		if(!(*it)->scheduledForInit)
			_scheduleInit((*it));
	}

	while(!_initQueue.empty()) {
		SharedObject *object = _initQueue.front();
		if(!object->wasInitialized)
			doInitialize(object);

		_initQueue.removeFront();
	}
}

// TODO: Use an explicit vector to reduce stack usage to O(1)?
void Loader::_scheduleInit(SharedObject *object) {
	// Here we detect cyclic dependencies.
	assert(!object->onInitStack);
	object->onInitStack = true;

	assert(!object->scheduledForInit);
	object->scheduledForInit = true;

	for(size_t i = 0; i < object->dependencies.size(); i++) {
		if(!object->dependencies[i]->scheduledForInit)
			_scheduleInit(object->dependencies[i]);
	}

	_initQueue.addBack(object);
	object->onInitStack = false;
}

void Loader::_processRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

	// copy relocations have to be performed after all other relocations
	if(type == R_X86_64_COPY)
		return;
	
	// resolve the symbol if there is a symbol
	frigg::Optional<ObjectSymbol> p;
	if(symbol_index) {
		auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
				+ symbol_index * sizeof(Elf64_Sym));
		ObjectSymbol r(object, symbol);
		p = object->loadScope->resolveSymbol(r, 0);
		if(!p) {
			if(ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
				frigg::panicLogger() << "Unresolved load-time symbol "
						<< r.getString() << " in object " << object->name << frigg::endLog;
			
			if(verbose)
				frigg::infoLogger() << "rtdl: Unresolved weak load-time symbol "
						<< r.getString() << " in object " << object->name << frigg::endLog;
		}
	}

	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	switch(type) {
	case R_X86_64_64: {
		assert(symbol_index);
		uint64_t symbol_addr = p ? p->virtualAddress() : 0;
		*((uint64_t *)rel_addr) = symbol_addr + reloc->r_addend;
	} break;
	case R_X86_64_GLOB_DAT: {
		assert(symbol_index);
		assert(!reloc->r_addend);
		uint64_t symbol_addr = p ? p->virtualAddress() : 0;
		*((uint64_t *)rel_addr) = symbol_addr;
	} break;
	case R_X86_64_RELATIVE: {
		assert(!symbol_index);
		*((uint64_t *)rel_addr) = object->baseAddress + reloc->r_addend;
	} break;
	// DTPMOD and DTPOFF are dynamic TLS relocations (for __tls_get_addr()).
	// TPOFF is a relocation to the initial TLS model.
	case R_X86_64_DTPMOD64: {
		assert(!reloc->r_addend);
		if(symbol_index) {
			assert(p);
			*((uint64_t *)rel_addr) = (uint64_t)p->object();
		}else{
			// TODO: is this behaviour actually documented anywhere?
			frigg::infoLogger() << "rtdl: Warning: DTPOFF64 with no symbol"
					" in object " << object->name << frigg::endLog;
			*((uint64_t *)rel_addr) = (uint64_t)object;
		}
	} break;
	case R_X86_64_DTPOFF64: {
		assert(p);
		assert(!reloc->r_addend);
		assert(p->object()->tlsModel == TlsModel::initial);
		*((uint64_t *)rel_addr) = p->symbol()->st_value;
	} break;
	case R_X86_64_TPOFF64: {
		if(symbol_index) {
			assert(p);
			assert(!reloc->r_addend);
			if(p->object()->tlsModel != TlsModel::initial)
				frigg::panicLogger() << "rtdl: In object " << object->name
						<< ": Static TLS relocation to dynamically loaded object "
						<< p->object()->name << frigg::endLog;
			*((uint64_t *)rel_addr) = p->object()->tlsOffset + p->symbol()->st_value;
		}else{
			assert(!reloc->r_addend);
			frigg::infoLogger() << "rtdl: Warning: TPOFF64 with no symbol"
					" in object " << object->name << frigg::endLog;
			if(object->tlsModel != TlsModel::initial)
				frigg::panicLogger() << "rtdl: In object " << object->name
						<< ": Static TLS relocation to dynamically loaded object "
						<< object->name << frigg::endLog;
			*((uint64_t *)rel_addr) = object->tlsOffset;
		}
	} break;
	default:
		frigg::panicLogger() << "Unexpected relocation type "
				<< (void *)type << frigg::endLog;
	}
}

void Loader::_processStaticRelocations(SharedObject *object) {
	frigg::Optional<uintptr_t> rela_offset;
	frigg::Optional<size_t> rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			break;
		case DT_RELAENT:
			assert(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(rela_offset && rela_length) {
		for(size_t offset = 0; offset < *rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + *rela_offset + offset);
			_processRela(object, reloc);
		}
	}else{
		assert(!rela_offset && !rela_length);
	}
}

void Loader::_processLazyRelocations(SharedObject *object) {
	if(object->globalOffsetTable == nullptr) {
		assert(object->lazyRelocTableOffset == 0);
		return;
	}
	object->globalOffsetTable[1] = object;
	object->globalOffsetTable[2] = (void *)&pltRelocateStub;
	
	if(!object->lazyTableSize)
		return;

	// adjust the addresses of JUMP_SLOT relocations
	assert(object->lazyExplicitAddend);
	for(size_t offset = 0; offset < object->lazyTableSize; offset += sizeof(Elf64_Rela)) {
		auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset + offset);
		Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
		Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
		uintptr_t rel_addr = object->baseAddress + reloc->r_offset;

		assert(type == R_X86_64_JUMP_SLOT);
		if(eagerBinding) {
			auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
					+ symbol_index * sizeof(Elf64_Sym));
			ObjectSymbol r(object, symbol);
			frigg::Optional<ObjectSymbol> p = object->loadScope->resolveSymbol(r, 0);
			if(!p) {
				if(ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
					frigg::panicLogger() << "rtdl: Unresolved JUMP_SLOT symbol "
							<< r.getString() << " in object " << object->name << frigg::endLog;
				
				if(verbose)
					frigg::infoLogger() << "rtdl: Unresolved weak JUMP_SLOT symbol "
							<< r.getString() << " in object " << object->name << frigg::endLog;
				*((uint64_t *)rel_addr) = 0;
			}else{
				*((uint64_t *)rel_addr) = p->virtualAddress();
			}
		}else{
			*((uint64_t *)rel_addr) += object->baseAddress;
		}
	}
}

