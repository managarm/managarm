
#include <stdint.h>

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <frg/string.hpp>
#include <frigg/elf.hpp>
#include <frigg/debug.hpp>
#include <thor-internal/descriptor.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/service_helpers.hpp>
#include <thor-internal/stream.hpp>

#include "mbus.frigg_pb.hpp"
#include "kernlet.frigg_pb.hpp"

namespace thor {

namespace {
	constexpr bool logBinding = false;
	constexpr bool logIo = false;
}

extern frigg::LazyInitializer<LaneHandle> mbusClient;

// ------------------------------------------------------------------------
// KernletObject class.
// ------------------------------------------------------------------------

KernletObject::KernletObject(void *entry,
		const frg::vector<KernletParameterType, KernelAlloc> &bind_types)
: _entry(entry), _bindDefns{*kernelAlloc}, _instanceSize{0} {
	for(auto type : bind_types) {
		if(type == KernletParameterType::offset) {
			_instanceSize = (_instanceSize + 3) & ~size_t(3);
			_bindDefns.push_back({type, _instanceSize});
			_instanceSize += 4;
		}else if(type == KernletParameterType::memoryView) {
			_instanceSize = (_instanceSize + 7) & ~size_t(7);
			_bindDefns.push_back({type, _instanceSize});
			_instanceSize += 8;
		}else if(type == KernletParameterType::bitsetEvent) {
			_instanceSize = (_instanceSize + 7) & ~size_t(7);
			_bindDefns.push_back({type, _instanceSize});
			_instanceSize += 8;
		}else{
			assert(!"Unexpected kernlet parameter type");
		}
	}
}

size_t KernletObject::instanceSize() {
	return _instanceSize;
}

size_t KernletObject::numberOfBindParameters() {
	return _bindDefns.size();
}

const KernletParameterDefn &KernletObject::defnOfBindParameter(size_t index) {
	return _bindDefns[index];
}

// ------------------------------------------------------------------------
// BoundKernlet class.
// ------------------------------------------------------------------------

BoundKernlet::BoundKernlet(frigg::SharedPtr<KernletObject> object)
: _object{std::move(object)} {
	_instance = reinterpret_cast<char *>(kernelAlloc->allocate(_object->instanceSize()));
}

void BoundKernlet::setupOffsetBinding(size_t index, uint32_t offset) {
	assert(index < _object->numberOfBindParameters());
	const auto &defn = _object->defnOfBindParameter(index);
	if(logBinding)
		frigg::infoLogger() << "thor: Binding offset " << offset
				<< " to instance offset " << defn.offset << frigg::endLog;
	memcpy(_instance + defn.offset, &offset, sizeof(uint32_t));
}

void BoundKernlet::setupMemoryViewBinding(size_t index, void *p) {
	assert(index < _object->numberOfBindParameters());
	const auto &defn = _object->defnOfBindParameter(index);
	if(logBinding)
		frigg::infoLogger() << "thor: Binding memory view " << p
				<< " to instance offset " << defn.offset << frigg::endLog;
	memcpy(_instance + defn.offset, &p, sizeof(void *));
}

void BoundKernlet::setupBitsetEventBinding(size_t index, frigg::SharedPtr<BitsetEvent> event) {
	assert(index < _object->numberOfBindParameters());
	const auto &defn = _object->defnOfBindParameter(index);
	if(logBinding)
		frigg::infoLogger() << "thor: Binding bitset event " << (void *)event.get()
				<< " to instance offset " << defn.offset << frigg::endLog;
	auto p = event.get();
	memcpy(_instance + defn.offset, &p, sizeof(void *));
}

int BoundKernlet::invokeIrqAutomation() {
	auto entry = reinterpret_cast<int (*)(const void *)>(_object->_entry);
	return entry(_instance);
}

// ------------------------------------------------------------------------
// kernletctl interface to user space.
// ------------------------------------------------------------------------

namespace {

frigg::SharedPtr<KernletObject> processElfDso(const char *buffer,
		const frg::vector<KernletParameterType, KernelAlloc> &bind_types) {
	auto base = reinterpret_cast<char *>(KernelVirtualMemory::global().allocate(0x10000));

	// Check the EHDR file header.
	Elf64_Ehdr ehdr;
	memcpy(&ehdr, buffer, sizeof(Elf64_Ehdr));
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');

	// Load all PHDRs.
	Elf64_Dyn *dynamic = nullptr;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(&phdr, buffer + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(Elf64_Phdr));

		if(phdr.p_type == PT_LOAD) {
			uintptr_t misalign = phdr.p_vaddr & (kPageSize - 1);
			assert(phdr.p_memsz > 0);

			// Map pages for the segment.
			// TODO: We need write permission to fill the page. Get rid of it.
			uint32_t pf = page_access::write;
			if(phdr.p_flags & PF_X)
				pf |= page_access::execute;

			for(size_t pg = 0; pg < misalign + phdr.p_memsz; pg += kPageSize) {
				auto va = reinterpret_cast<VirtualAddr>(base + phdr.p_vaddr + pg)
						& ~(kPageSize - 1);
				auto physical = physicalAllocator->allocate(kPageSize);
				assert(physical != PhysicalAddr(-1) && "OOM");
				KernelPageSpace::global().mapSingle4k(va, physical,
						pf, CachingMode::null);
			}

			// Fill the segment.
			memset(base + phdr.p_vaddr, 0, phdr.p_memsz);
			memcpy(base + phdr.p_vaddr, buffer + phdr.p_offset, phdr.p_filesz);
		}else if(phdr.p_type == PT_DYNAMIC) {
			dynamic = reinterpret_cast<Elf64_Dyn *>(base + phdr.p_vaddr);
		}else if(phdr.p_type == PT_NOTE
				|| phdr.p_type == PT_GNU_EH_FRAME
				|| phdr.p_type == PT_GNU_STACK
				|| phdr.p_type == PT_GNU_RELRO) {
			// Ignore the PHDR.
		}else{
			assert(!"Unexpected PHDR");
		}
	}

	// Extract symbol & relocation tables from DYNAMIC.
	const char *str_tab = nullptr;
	const Elf64_Sym *sym_tab = nullptr;
	const Elf64_Word *hash_tab = nullptr;
	const char *plt_rels = nullptr;
	size_t plt_rel_sectionsize = 0;

	for(size_t i = 0; dynamic[i].d_tag != DT_NULL; i++) {
		auto ent = dynamic + i;
		switch(ent->d_tag) {
		// References to sections that we need to extract:
		case DT_STRTAB:
			str_tab = base + ent->d_ptr;
			break;
		case DT_SYMTAB:
			sym_tab = reinterpret_cast<const Elf64_Sym *>(base + ent->d_ptr);
			break;
		case DT_HASH:
			hash_tab = reinterpret_cast<const Elf64_Word *>(base + ent->d_ptr);
			break;
		case DT_JMPREL:
			plt_rels = base + ent->d_ptr;
			break;

		// Data that we need to extract:
		case DT_PLTRELSZ:
			plt_rel_sectionsize = ent->d_val;
			break;

		// Make sure those entries match our expectation:
		case DT_SYMENT:
			assert(ent->d_val == sizeof(Elf64_Sym));
			break;

		// Ignore the following entries:
		case DT_STRSZ:
		case DT_PLTGOT:
		case DT_PLTREL:
		case DT_GNU_HASH:
			break;
		default:
			assert(!"Unexpected dynamic entry in kernlet");
		}
	}
	assert(str_tab);
	assert(sym_tab);
	assert(hash_tab);

	// Perform relocations.
	auto resolveExternal = [] (frg::string_view name) -> void * {
		uint16_t (*abi_pio_read16)(ptrdiff_t) =
			[] (ptrdiff_t offset) -> uint16_t {
				if(logIo)
					frigg::infoLogger() << "__pio_read16 on offset: " << offset << frigg::endLog;
				auto value = arch::io_ops<uint16_t>::load(offset);
				if(logIo)
					frigg::infoLogger() << "    Read " << (unsigned int)value << frigg::endLog;
				return value;
			};

		void (*abi_pio_write16)(ptrdiff_t, uint16_t) =
			[] (ptrdiff_t offset, uint16_t value) {
				if(logIo)
					frigg::infoLogger() << "__pio_write16 on offset: " << offset << frigg::endLog;
				arch::io_ops<uint16_t>::store(offset, value);
				if(logIo)
					frigg::infoLogger() << "    Wrote " << value << frigg::endLog;
			};

		uint8_t (*abi_mmio_read8)(const char *, ptrdiff_t) =
			[] (const char *base, ptrdiff_t offset) -> uint8_t {
				if(logIo)
					frigg::infoLogger() << "__mmio_read8 on " << (void *)base
							<< ", offset: " << offset << frigg::endLog;
				auto p = reinterpret_cast<const uint8_t *>(base + offset);
				auto value = arch::mem_ops<uint8_t>::load(p);
				if(logIo)
					frigg::infoLogger() << "    Read " << (unsigned int)value << frigg::endLog;
				return value;
			};
		uint32_t (*abi_mmio_read32)(const char *, ptrdiff_t) =
			[] (const char *base, ptrdiff_t offset) -> uint32_t {
				if(logIo)
					frigg::infoLogger() << "__mmio_read32 on " << (void *)base
							<< ", offset: " << offset << frigg::endLog;
				auto p = reinterpret_cast<const uint32_t *>(base + offset);
				auto value = arch::mem_ops<uint32_t>::load(p);
				if(logIo)
					frigg::infoLogger() << "    Read " << value << frigg::endLog;
				return value;
			};

		void (*abi_mmio_write32)(char *, ptrdiff_t, uint32_t) =
			[] (char *base, ptrdiff_t offset, uint32_t value) {
				if(logIo)
					frigg::infoLogger() << "__mmio_write32 on " << (void *)base
							<< ", offset: " << offset << frigg::endLog;
				auto p = reinterpret_cast<uint32_t *>(base + offset);
				arch::mem_ops<uint32_t>::store(p, value);
				if(logIo)
					frigg::infoLogger() << "    Wrote " << value << frigg::endLog;
			};

		void (*abi_trigger_bitset)(void *, uint32_t) =
			[] (void *p, uint32_t bits) {
				if(logIo)
					frigg::infoLogger() << "__trigger_bitset on "
							<< p << ", bits: " << bits << frigg::endLog;
				auto event = static_cast<BitsetEvent *>(p);
				event->trigger(bits);
			};

		if(name == "__pio_read16")
			return reinterpret_cast<void *>(abi_pio_read16);
		else if(name == "__pio_write16")
			return reinterpret_cast<void *>(abi_pio_write16);
		else if(name == "__mmio_read8")
			return reinterpret_cast<void *>(abi_mmio_read8);
		else if(name == "__mmio_read32")
			return reinterpret_cast<void *>(abi_mmio_read32);
		else if(name == "__mmio_write32")
			return reinterpret_cast<void *>(abi_mmio_write32);
		else if(name == "__trigger_bitset")
			return reinterpret_cast<void *>(abi_trigger_bitset);
		frigg::panicLogger() << "Could not resolve external " << name.data() << frigg::endLog;
		__builtin_unreachable();
	};

	for(size_t off = 0; off < plt_rel_sectionsize; off += sizeof(Elf64_Rela)) {
		auto reloc = reinterpret_cast<const Elf64_Rela *>(plt_rels + off);
		assert(ELF64_R_TYPE(reloc->r_info) == R_X86_64_JUMP_SLOT);

		auto rp = reinterpret_cast<uint64_t *>(base + reloc->r_offset);
		auto symbol = sym_tab + ELF64_R_SYM(reloc->r_info);
		auto sym_name = frg::string_view{str_tab + symbol->st_name};
		*rp = reinterpret_cast<uint64_t>(resolveExternal(sym_name));
	}

	// Look up symbols.
	auto elf64Hash = [] (frg::string_view string) -> uint32_t {
		uint32_t h = 0;
		for(size_t i = 0; i < string.size(); ++i) {
			h = (h << 4) + (uint8_t)string[i];
			uint32_t g = h & 0xF0000000;
			if(g)
				h ^= g >> 24;
			h &= 0x0FFFFFFF;
		}
		return h;
	};

	auto eligible = [&] (const Elf64_Sym *candidate) {
		if(candidate->st_shndx == SHN_UNDEF)
			return false;
		auto bind = ELF64_ST_BIND(candidate->st_info);
		if(bind != STB_GLOBAL && bind != STB_WEAK)
			return false;
		return true;
	};

	auto lookup = [&] (frg::string_view name) -> void * {
		auto n = hash_tab[0]; // Number of buckets.
		auto b = elf64Hash(name) % n; // First bucket the symbol can appear in.
		for(auto idx = hash_tab[2 + b]; idx; idx = hash_tab[2 + n + idx]) {
			auto candidate = sym_tab + idx;
			auto cand_name = frg::string_view{str_tab + candidate->st_name};
			if(!eligible(candidate) || cand_name != name)
				continue;
			return base + candidate->st_value;
		}
		frigg::panicLogger() << "thor: Unable to resolve kernel symbol '"
				<< name.data() << "'" << frigg::endLog;
		__builtin_unreachable();
	};

	auto entry = lookup("automate_irq");
	return frigg::makeShared<KernletObject>(*kernelAlloc, entry, bind_types);
}

coroutine<Error> handleReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	if(reqError != Error::success)
		co_return reqError;
	managarm::kernlet::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::kernlet::CntReqType::UPLOAD) {
		frg::vector<KernletParameterType, KernelAlloc> bind_types{*kernelAlloc};
		for(size_t i = 0; i < req.bind_types_size(); i++) {
			switch(req.bind_types(i)) {
			case managarm::kernlet::ParameterType::OFFSET:
				bind_types.push_back(KernletParameterType::offset);
				break;
			case managarm::kernlet::ParameterType::MEMORY_VIEW:
				bind_types.push_back(KernletParameterType::memoryView);
				break;
			case managarm::kernlet::ParameterType::BITSET_EVENT:
				bind_types.push_back(KernletParameterType::bitsetEvent);
				break;
			default:
				assert(!"Unexpected kernlet parameter type");
			}
		}

		auto [elfError, elfBuffer] = co_await RecvBufferSender{lane};
		if(elfError != Error::success)
			co_return elfError;
		auto kernlet = processElfDso(reinterpret_cast<char *>(elfBuffer.data()), bind_types);

		managarm::kernlet::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kernlet::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success)
			co_return respError;
		auto objectError = co_await PushDescriptorSender{lane,
				KernletObjectDescriptor{std::move(kernlet)}};
		if(objectError != Error::success)
			co_return objectError;
	}else{
		managarm::kernlet::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kernlet::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success)
			co_return respError;
	}

	co_return Error::success;
}

} // anonymous namespace

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

coroutine<void> handleBind(LaneHandle objectLane);

coroutine<void> createObject(LaneHandle mbusLane) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(offerError == Error::success && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "kernletctl"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleBind(objectLane);
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(acceptError == Error::success && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(respError == Error::success && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(boundError == Error::success && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleReq(boundLane);
			if(error == Error::endOfLane)
				break;
			if(isRemoteIpcError(error))
				frigg::infoLogger() << "thor: Aborting svrctl request"
						" after remote violated the protocol" << frigg::endLog;
			assert(error == Error::success);
		}
	})(boundLane));
}

} // anonymous namespace

void initializeKernletCtl() {
	// Create a fiber to manage requests to the kernletctl mbus object.
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, createObject(*mbusClient));
	});
}

} // namespace thor

