#include <eir-internal/cpio.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/arch.hpp>

#include <frg/utility.hpp>
#include <frg/manual_box.hpp>
#include <frg/array.hpp>
#include <elf.h>
#include <physical-buddy.hpp>

namespace eir {

void *initrd = nullptr;
address_t allocatedMemory;
frg::span<uint8_t> kernel_image{nullptr, 0};
frg::span<uint8_t> initrd_image{nullptr, 0};

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

Region regions[numRegions];

Region *obtainRegion() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::null)
			continue;
		regions[i].regionType = RegionType::unconstructed;
		return &regions[i];
	}
	eir::panicLogger() << "Eir: Memory region limit exhausted" << frg::endlog;
	__builtin_unreachable();
}

void createInitialRegion(address_t base, address_t size) {
	auto limit = base + size;

	address_t address = base;

	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~address_t(0x1FFFFF);

	if(address >= limit) {
		eir::infoLogger() << "eir: Discarding memory region at 0x"
				<< frg::hex_fmt{base} << " (smaller than alignment)" << frg::endlog;
		return;
	}

	// Trash the initial memory to find bugs in thor.
	// TODO: This code fails for size > 2^32.
	/*
	auto accessor = reinterpret_cast<uint8_t *>(address);
	uint64_t pattern = 0xB306'94E7'F8D2'78AB;
	for(ptrdiff_t i = 0; i < limit - address; ++i) {
		accessor[i] = pattern;
		pattern = (pattern << 8) | (pattern >> 56);
	}
	asm volatile ("" : : : "memory");
	*/

	// For now we ensure that the kernel has some memory to work with.
	// TODO: Handle small memory regions.
	if(limit - address < 32 * address_t(0x100000)) {
		eir::infoLogger() << "eir: Discarding memory region at 0x"
				<< frg::hex_fmt{base} << " (smaller than minimum size)" << frg::endlog;
		return;
	}

	assert(!(address % pageSize));
	assert(!(limit % pageSize));

	auto region = obtainRegion();
	region->regionType = RegionType::allocatable;
	region->address = address;
	region->size = limit - address;
}

void createInitialRegions(InitialRegion region, frg::span<InitialRegion> reserved) {
	if (!reserved.size()) {
		createInitialRegion((region.base + 0xFFF) & ~0xFFF, region.size & ~0xFFF);
	} else {
		auto rsv = reserved.data()[0];

		if (rsv.base > (region.base + region.size)
				|| (rsv.base + rsv.size) < region.base) {
			createInitialRegions(region, {reserved.data() + 1, reserved.size() - 1});
			return;
		}

		if (rsv.base > region.base) {
			createInitialRegions({region.base, rsv.base - region.base}, {reserved.data() + 1, reserved.size() - 1});
		}

		if (rsv.base + rsv.size < region.base + region.size) {
			createInitialRegions({rsv.base + rsv.size, region.base + region.size - (rsv.base + rsv.size)}, {reserved.data() + 1, reserved.size() - 1});
		}
	}
}

address_t cutFromRegion(size_t size) {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		if(regions[i].size < size)
			continue;

		regions[i].size -= size;

		// Discard this region if it's smaller than alignment
		if(regions[i].size < 0x200000)
			regions[i].regionType = RegionType::null;

		return regions[i].address + regions[i].size;
	}

	eir::panicLogger() << "Eir: Unable to cut memory from a region" << frg::endlog;
	__builtin_unreachable();
}

void setupRegionStructs() {
	for(size_t j = numRegions; j > 0; j--) {
		size_t i = j - 1;

		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto preRoots = regions[i].size >> (pageShift + order);
		auto overhead = BuddyAccessor::determineSize(preRoots, order);
		overhead = (overhead + address_t(pageSize - 1)) & ~address_t(pageSize - 1);

		assert(overhead >= preRoots * (1 << (order + 1)));

		regions[i].buddyTree = cutFromRegion(overhead);
		regions[i].buddyOverhead = overhead;
	}

	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto numRoots = regions[i].size >> (pageShift + order);
		assert(numRoots >= 32);

		assert(regions[i].size / 4096 >= numRoots * (1 << order));

		regions[i].order = order;
		regions[i].numRoots = numRoots;

		// Finally initialize the buddy tree.
		auto tablePtr = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		BuddyAccessor::initialize(tablePtr, numRoots, order);
	}
}

// ----------------------------------------------------------------------------

uintptr_t bootReserve(size_t length, size_t alignment) {
	assert(length <= pageSize);
	assert(alignment <= pageSize);

	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		BuddyAccessor accessor{regions[i].address, pageShift,
				table, regions[i].numRoots, regions[i].order};
		auto physical = accessor.allocate(0, 32);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
		return physical;
	}

	eir::panicLogger() << "Eir: Out of memory" << frg::endlog;
	__builtin_unreachable();
}

uintptr_t allocPage() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		BuddyAccessor accessor{regions[i].address, pageShift,
				table, regions[i].numRoots, regions[i].order};
		auto physical = accessor.allocate(0, 32);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
		allocatedMemory += pageSize;
		return physical;
	}

	eir::panicLogger() << "Eir: Out of memory" << frg::endlog;
	__builtin_unreachable();
}

// ----------------------------------------------------------------------------

#ifdef EIR_KASAN
namespace {
	constexpr int kasanShift = 3;
	constexpr address_t kasanShadowDelta = 0xdfffe00000000000;

	constexpr size_t kasanScale = size_t{1} << kasanShift;

	address_t kasanToShadow(address_t address) {
		return kasanShadowDelta + (address >> kasanShift);
	}

	void setShadowRange(address_t base, size_t size, int8_t value) {
		assert(!(base & (kasanScale - 1)));
		assert(!(size & (kasanScale - 1)));

		size_t progress = 0;
		while(progress < size) {
			auto shadow = kasanToShadow(base + progress);
			auto page = shadow & ~address_t{pageSize - 1};
			auto physical = getSingle4kPage(page);
			assert(physical != static_cast<address_t>(-1));

			auto p = reinterpret_cast<int8_t *>(physical);
			auto n = shadow & (pageSize - 1);
			while(n < pageSize && progress < size) {
				assert(p[n] == static_cast<int8_t>(0xFF));
				p[n] = value;
				++n;
				progress += kasanScale;
			}
		}
	};

	void setShadowByte(address_t address, int8_t value) {
		assert(!(address & (kasanScale - 1)));

		auto shadow = kasanToShadow(address);
		auto page = shadow & ~address_t{pageSize - 1};
		auto physical = getSingle4kPage(page);
		assert(physical != static_cast<address_t>(-1));

		auto p = reinterpret_cast<int8_t *>(physical);
		auto n = shadow & (pageSize - 1);
		assert(p[n] == static_cast<int8_t>(0xFF));
		p[n] = value;
	};
}
#endif // EIR_KASAN

void mapKasanShadow(address_t base, size_t size) {
#ifdef EIR_KASAN
	assert(!(base & (kasanScale - 1)));

	eir::infoLogger() << "eir: Mapping KASAN shadow for 0x" << frg::hex_fmt{base}
			<< ", size: 0x" << frg::hex_fmt{size} << frg::endlog;

	size = (size + kasanScale - 1) & ~(kasanScale - 1);

	for(address_t page = (kasanToShadow(base) & ~address_t{pageSize - 1});
			page < ((kasanToShadow(base + size) + pageSize - 1) & ~address_t{pageSize - 1});
			page += pageSize) {
		auto physical = getSingle4kPage(page);
		if(physical != static_cast<address_t>(-1))
			continue;
		physical = allocPage();
		memset(reinterpret_cast<void *>(physical), 0xFF, pageSize);
		mapSingle4kPage(page, physical, PageFlags::write | PageFlags::global);
	}
#else
	(void)base;
	(void)size;
#endif // EIR_KASAN
}

void unpoisonKasanShadow(address_t base, size_t size) {
#ifdef EIR_KASAN
	assert(!(base & (kasanScale - 1)));

	eir::infoLogger() << "eir: Unpoisoning KASAN shadow for 0x" << frg::hex_fmt{base}
			<< ", size: 0x" << frg::hex_fmt{size} << frg::endlog;

	setShadowRange(base, size & ~(kasanScale - 1), 0);
	if(size & (kasanScale - 1))
		setShadowByte(base + (size & ~(kasanScale - 1)), size & (kasanScale - 1));
#else
	(void)base;
	(void)size;
#endif // EIR_KASAN
}

// ----------------------------------------------------------------------------

void mapRegionsAndStructs() {
	// This region should be available RAM on every PC.
	for(size_t page = 0x8000; page < 0x80000; page += pageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + page,
					page, PageFlags::write | PageFlags::global);
	mapKasanShadow(0xFFFF'8000'0000'8000, 0x80000);
	unpoisonKasanShadow(0xFFFF'8000'0000'8000, 0x80000);

	address_t treeMapping = 0xFFFF'C080'0000'0000;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Map the region itself.
		for(address_t page = 0; page < regions[i].size; page += pageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + regions[i].address + page,
					regions[i].address + page, PageFlags::write | PageFlags::global);
		mapKasanShadow(0xFFFF'8000'0000'0000 + regions[i].address, regions[i].size);
		unpoisonKasanShadow(0xFFFF'8000'0000'0000 + regions[i].address, regions[i].size);

		// Map the buddy tree.
		address_t buddyMapping = treeMapping;
		treeMapping += regions[i].buddyOverhead;

		for(address_t page = 0; page < regions[i].buddyOverhead; page += pageSize) {
			mapSingle4kPage(buddyMapping + page,
					regions[i].buddyTree + page, PageFlags::write | PageFlags::global);
		}
		mapKasanShadow(buddyMapping, regions[i].buddyOverhead);
		unpoisonKasanShadow(buddyMapping, regions[i].buddyOverhead);
		regions[i].buddyMap = buddyMapping;
	}
}

void allocLogRingBuffer() {
	// 256 MiB
	for (size_t i = 0; i < 0x1000'0000; i += pageSize)
		mapSingle4kPage(0xFFFF'F000'0000'0000 + i,
				allocPage(), PageFlags::write | PageFlags::global);
	mapKasanShadow(0xFFFF'F000'0000'0000, 0x1000'0000);
	unpoisonKasanShadow(0xFFFF'F000'0000'0000, 0x1000'0000);
}

// ----------------------------------------------------------------------------
// Bootstrap information handling.
// ----------------------------------------------------------------------------

address_t bootstrapDataPointer = 0xFFFF'FE80'0001'0000;

address_t mapBootstrapData(void *p) {
	auto pointer = bootstrapDataPointer;
	bootstrapDataPointer += pageSize;
	mapSingle4kPage(pointer, (address_t)p, 0);
	mapKasanShadow(pointer, pageSize);
	unpoisonKasanShadow(pointer, pageSize);
	return pointer;
}

// ----------------------------------------------------------------------------

uint64_t kernelEntry;

void parseInitrd(void *initrd) {
	CpioRange cpio_range{reinterpret_cast<void *>(initrd)};
	auto initrd_end = reinterpret_cast<uintptr_t>(cpio_range.eof());
	eir::infoLogger() << "Initrd ends at " << (void *)initrd_end << frg::endlog;
	initrd_image = frg::span<uint8_t>{
		reinterpret_cast<uint8_t *>(initrd),
		initrd_end - reinterpret_cast<uintptr_t>(initrd)};

	for(auto entry : cpio_range) {
		if(entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	if(!kernel_image.data() || !kernel_image.size())
		eir::panicLogger() << "eir: could not find thor in the initrd.cpio" << frg::endlog;
}

address_t loadKernelImage(void *image) {
	Elf64_Ehdr ehdr;
	memcpy(&ehdr, image, sizeof(Elf64_Ehdr));
	if(ehdr.e_ident[0] != '\x7F'
			|| ehdr.e_ident[1] != 'E'
			|| ehdr.e_ident[2] != 'L'
			|| ehdr.e_ident[3] != 'F') {
		eir::panicLogger() << "Illegal magic fields" << frg::endlog;
	}
	assert(ehdr.e_type == ET_EXEC);

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(&phdr, (void *)((uintptr_t)image
				+ (uintptr_t)ehdr.e_phoff
				+ i * ehdr.e_phentsize),
				sizeof(Elf64_Phdr));
		if(phdr.p_type != PT_LOAD)
			continue;
		assert(!(phdr.p_offset & (pageSize - 1)));
		assert(!(phdr.p_vaddr & (pageSize - 1)));

		uint32_t map_flags = PageFlags::global;
		if((phdr.p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
			// no additional flags
		}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= PageFlags::write;
		}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= PageFlags::execute;
		}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W | PF_X)) {
			eir::infoLogger() << "eir: warning: Mapping PHDR with RWX permissions" << frg::endlog;
			map_flags |= PageFlags::write | PageFlags::execute;
		}else{
			eir::panicLogger() << "Illegal combination of segment permissions"
					<< frg::endlog;
		}

		uintptr_t pg = 0;
		while(pg < (uintptr_t)phdr.p_memsz) {
			auto backing = allocPage();
			memset(reinterpret_cast<void *>(backing), 0, pageSize);
			if(pg < (uintptr_t)phdr.p_filesz)
				memcpy(reinterpret_cast<void *>(backing),
						reinterpret_cast<void *>((uintptr_t)image + (uintptr_t)phdr.p_offset + pg),
						frg::min(pageSize, (uintptr_t)phdr.p_filesz - pg));
			mapSingle4kPage(phdr.p_vaddr + pg, backing, map_flags);
			pg += pageSize;
		}
		mapKasanShadow(phdr.p_paddr, phdr.p_memsz);
		unpoisonKasanShadow(phdr.p_paddr, phdr.p_memsz);
	}

	kernelEntry = ehdr.e_entry;
	return ehdr.e_entry;
}

EirInfo *generateInfo(frg::string_view cmdline){
	// Setup the eir interface struct.
	auto info_ptr = bootAlloc<EirInfo>();
	memset(info_ptr, 0, sizeof(EirInfo));
	auto info_vaddr = mapBootstrapData(info_ptr);
	assert(info_vaddr == 0xFFFF'FE80'0001'0000);
	info_ptr->signature = eirSignatureValue;

	// Pass all memory regions to thor.
	int n = 0;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::allocatable)
			n++;
	}

	auto regionInfos = bootAlloc<EirRegion>(n);
	info_ptr->numRegions = n;
	info_ptr->regionInfo = mapBootstrapData(regionInfos);
	int j = 0;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		regionInfos[j].address = regions[i].address;
		regionInfos[j].length = regions[i].size;
		regionInfos[j].order = regions[i].order;
		regionInfos[j].numRoots = regions[i].numRoots;
		regionInfos[j].buddyTree = regions[i].buddyMap;
		j++;
	}

	// Parse the kernel command line.
	const char *l = cmdline.data();
	while(true) {
		while(*l && *l == ' ')
			l++;
		if(!(*l))
			break;

		const char *s = l;
		while(*s && *s != ' ')
			s++;

		frg::string_view token{l, static_cast<size_t>(s - l)};
		if(token == "serial") {
			info_ptr->debugFlags |= eirDebugSerial;
		}else if(token == "bochs") {
			info_ptr->debugFlags |= eirDebugBochs;
		}else if(token == "kernel-profile") {
			info_ptr->debugFlags |= eirDebugKernelProfile;
		}
		l = s;
	}

	auto cmd_length = cmdline.size();
	assert(cmd_length <= pageSize);
	auto cmd_buffer = bootAlloc<char>(cmd_length);
	memcpy(cmd_buffer, cmdline.data(), cmd_length + 1);
	info_ptr->commandLine = mapBootstrapData(cmd_buffer);

	return info_ptr;
}

} // namespace eir
