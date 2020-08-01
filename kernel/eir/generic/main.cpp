#include <eir-internal/generic.hpp>
#include <eir-internal/arch.hpp>

#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/array.hpp>
#include <frigg/elf.hpp>
#include <frigg/libc.hpp>
#include <frigg/string.hpp>
#include <frigg/support.hpp>
#include <render-text.hpp>
#include <physical-buddy.hpp>

namespace eir {

address_t bootMemoryLimit;
address_t allocatedMemory;

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
	frigg::panicLogger() << "Eir: Memory region limit exhausted" << frigg::endLog;
	__builtin_unreachable();
}

void createInitialRegion(address_t base, address_t size) {
	auto limit = base + size;

	// For now we do not touch memory that is required during boot.
	address_t address = frigg::max(base, bootMemoryLimit);

	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~address_t(0x1FFFFF);

	if(address >= limit) {
		frigg::infoLogger() << "eir: Discarding memory region at 0x"
				<< frigg::logHex(base) << " (smaller than alignment)" << frigg::endLog;
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
		frigg::infoLogger() << "eir: Discarding memory region at 0x"
				<< frigg::logHex(base) << " (smaller than minimum size)" << frigg::endLog;
		return;
	}

	assert(!(address % pageSize));
	assert(!(limit % pageSize));

	auto region = obtainRegion();
	region->regionType = RegionType::allocatable;
	region->address = address;
	region->size = limit - address;
}

address_t cutFromRegion(size_t size) {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		if(regions[i].size < size)
			continue;

		regions[i].size -= size;
		return regions[i].address + regions[i].size;
	}
	
	frigg::panicLogger() << "Eir: Unable to cut memory from a region" << frigg::endLog;
	__builtin_unreachable();
}

void setupRegionStructs() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto pre_roots = regions[i].size >> (pageShift + order);
		auto overhead = BuddyAccessor::determineSize(pre_roots, order);
		overhead = (overhead + address_t(pageSize - 1)) & ~address_t(pageSize - 1);

		// Note that cutFromRegion might actually reduce this regions' size.
		auto table_paddr = cutFromRegion(overhead);
		auto num_roots = regions[i].size >> (pageShift + order);
		assert(num_roots >= 32);

		regions[i].order = order;
		regions[i].numRoots = num_roots;
		regions[i].buddyTree = table_paddr;
		regions[i].buddyOverhead = overhead;

		// Finally initialize the buddy tree.
		auto table_ptr = reinterpret_cast<int8_t *>(table_paddr);
		BuddyAccessor::initialize(table_ptr, num_roots, order);
	}
}

// ----------------------------------------------------------------------------

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

void *displayFb;
int displayWidth;
int displayHeight;
size_t displayPitch;
int outputX;
int outputY;

void setFbInfo(void *ptr, int width, int height, size_t pitch) {
	displayFb = ptr;
	displayWidth = width;
	displayHeight = height;
	displayPitch = pitch;
}

struct OutputSink {
	void print(char c);
	void print(const char *str);
};

void OutputSink::print(char c) {
	debugPrintChar(c);

	if(displayFb) {
		if(c == '\n') {
			outputX = 0;
			outputY++;
		}else if(outputX >= displayWidth / fontWidth) {
			outputX = 0;
			outputY++;
		}else if(outputY >= displayHeight / fontHeight) {
			// TODO: Scroll.
		}else{
			renderChars(displayFb, displayPitch / sizeof(uint32_t),
					outputX, outputY, &c, 1, 15, -1,
					std::integral_constant<int, fontWidth>{},
					std::integral_constant<int, fontHeight>{});
			outputX++;
		}
	}
}

void OutputSink::print(const char *str) {
	while(*str)
		print(*(str++));
}

OutputSink infoSink;

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

	frigg::panicLogger() << "Eir: Out of memory" << frigg::endLog;
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

	frigg::panicLogger() << "Eir: Out of memory" << frigg::endLog;
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

	frigg::infoLogger() << "eir: Mapping KASAN shadow for 0x" << frigg::logHex(base)
			<< ", size: 0x" << frigg::logHex(size) << frigg::endLog;

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
#endif // EIR_KASAN
}

void unpoisonKasanShadow(address_t base, size_t size) {
#ifdef EIR_KASAN
	assert(!(base & (kasanScale - 1)));

	frigg::infoLogger() << "eir: Unpoisoning KASAN shadow for 0x" << frigg::logHex(base)
			<< ", size: 0x" << frigg::logHex(size) << frigg::endLog;

	setShadowRange(base, size & ~(kasanScale - 1), 0);
	if(size & (kasanScale - 1))
		setShadowByte(base + (size & ~(kasanScale - 1)), size & (kasanScale - 1));
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
		auto buddyOverhead = BuddyAccessor::determineSize(regions[i].numRoots, regions[i].order);
		address_t buddyMapping = treeMapping;
		treeMapping += buddyOverhead;

		for(address_t page = 0; page < buddyOverhead; page += pageSize) {
			mapSingle4kPage(buddyMapping + page,
					regions[i].buddyTree + page, PageFlags::write | PageFlags::global);
		}
		mapKasanShadow(buddyMapping, buddyOverhead);
		unpoisonKasanShadow(buddyMapping, buddyOverhead);
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

address_t loadKernelImage(void *image) {
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)image;
	if(ehdr->e_ident[0] != '\x7F'
			|| ehdr->e_ident[1] != 'E'
			|| ehdr->e_ident[2] != 'L'
			|| ehdr->e_ident[3] != 'F') {
		frigg::panicLogger() << "Illegal magic fields" << frigg::endLog;
	}
	assert(ehdr->e_type == ET_EXEC);

	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)image
				+ (uintptr_t)ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		if(phdr->p_type != PT_LOAD)
			continue;
		assert(!(phdr->p_offset & (pageSize - 1)));
		assert(!(phdr->p_vaddr & (pageSize - 1)));

		uint32_t map_flags = PageFlags::global;
		if((phdr->p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
			// no additional flags
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= PageFlags::write;
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= PageFlags::execute;
		}else{
			frigg::panicLogger() << "Illegal combination of segment permissions"
					<< frigg::endLog;
		}

		uintptr_t pg = 0;
		while(pg < (uintptr_t)phdr->p_memsz) {
			auto backing = allocPage();
			memset(reinterpret_cast<void *>(backing), 0, pageSize);
			if(pg < (uintptr_t)phdr->p_filesz)
				memcpy(reinterpret_cast<void *>(backing),
						reinterpret_cast<void *>((uintptr_t)image + (uintptr_t)phdr->p_offset + pg),
						frigg::min(pageSize, (uintptr_t)phdr->p_filesz - pg));
			mapSingle4kPage(phdr->p_vaddr + pg, backing, map_flags);
			pg += pageSize;
		}
		mapKasanShadow(phdr->p_paddr, phdr->p_memsz);
		unpoisonKasanShadow(phdr->p_paddr, phdr->p_memsz);
	}

	return ehdr->e_entry;
}

EirInfo *generateInfo(const char* cmdline){
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
	const char *l = cmdline;
	while(true) {
		while(*l && *l == ' ')
			l++;
		if(!(*l))
			break;

		const char *s = l;
		while(*s && *s != ' ')
			s++;

		frigg::StringView token{l, static_cast<size_t>(s - l)};
		if(token == "serial") {
			info_ptr->debugFlags |= eirDebugSerial;
		}else if(token == "bochs") {
			info_ptr->debugFlags |= eirDebugBochs;
		}else if(token == "kernel-profile") {
			info_ptr->debugFlags |= eirDebugKernelProfile;
		}
		l = s;
	}

	auto cmd_length = strlen(cmdline);
	assert(cmd_length <= pageSize);
	auto cmd_buffer = bootAlloc<char>(cmd_length);
	memcpy(cmd_buffer, cmdline, cmd_length + 1);
	info_ptr->commandLine = mapBootstrapData(cmd_buffer);

	return info_ptr;
}

}

void friggBeginLog() { }
void friggEndLog() { }

void friggPrintCritical(char c) {
	eir::infoSink.print(c);
}

void friggPrintCritical(const char *str) {
	eir::infoSink.print(str);
}

void friggPanic() {
	while(true) { }
	__builtin_unreachable();
}

