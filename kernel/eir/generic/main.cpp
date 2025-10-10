#include <eir-internal/arch.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/cpio.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir-internal/uart/uart.hpp>

#include <elf.h>
#include <frg/array.hpp>
#include <frg/manual_box.hpp>
#include <frg/utility.hpp>
#include <limits.h>
#include <physical-buddy.hpp>

namespace eir {

// Address of the DTB.
constinit physaddr_t eirDtbPtr{0};
constinit physaddr_t eirRsdpAddr{0};
constinit physaddr_t eirSmbios3Addr{0};

void *initrd = nullptr;

address_t allocatedMemory;
frg::span<uint8_t> kernel_image{nullptr, 0};
address_t kernel_physical = SIZE_MAX;
frg::span<uint8_t> initrd_image{nullptr, 0};
// Start address of a physical map provided by the bootloader. Defaults to 0.
address_t physOffset = 0;

PerCpuRegion perCpuRegion{0, 0};

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

Region regions[numRegions];

Region *obtainRegion() {
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::null)
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

	if (address >= limit) {
		eir::infoLogger() << "eir: Discarding memory region at 0x" << frg::hex_fmt{base}
		                  << " (smaller than alignment)" << frg::endlog;
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
	if (limit - address < 32 * address_t(0x100000)) {
		eir::infoLogger() << "eir: Discarding memory region at 0x" << frg::hex_fmt{base}
		                  << " (smaller than minimum size)" << frg::endlog;
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

		if (rsv.base > (region.base + region.size) || (rsv.base + rsv.size) < region.base) {
			createInitialRegions(region, {reserved.data() + 1, reserved.size() - 1});
			return;
		}

		if (rsv.base > region.base) {
			createInitialRegions(
			    {region.base, rsv.base - region.base}, {reserved.data() + 1, reserved.size() - 1}
			);
		}

		if (rsv.base + rsv.size < region.base + region.size) {
			createInitialRegions(
			    {rsv.base + rsv.size, region.base + region.size - (rsv.base + rsv.size)},
			    {reserved.data() + 1, reserved.size() - 1}
			);
		}
	}
}

address_t cutFromRegion(size_t size) {
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		if (regions[i].size < size)
			continue;

		regions[i].size -= size;

		// Discard this region if it's smaller than alignment
		if (regions[i].size < 0x200000)
			regions[i].regionType = RegionType::null;

		return regions[i].address + regions[i].size;
	}

	eir::panicLogger() << "Eir: Unable to cut memory from a region" << frg::endlog;
	__builtin_unreachable();
}

void setupRegionStructs() {
	for (size_t j = numRegions; j > 0; j--) {
		size_t i = j - 1;

		if (regions[i].regionType != RegionType::allocatable)
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

	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto numRoots = regions[i].size >> (pageShift + order);
		assert(numRoots >= 32);

		assert(regions[i].size / 4096 >= numRoots * (1 << order));

		regions[i].order = order;
		regions[i].numRoots = numRoots;

		// Finally initialize the buddy tree.
		auto tablePtr = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor::initialize(tablePtr, numRoots, order);
	}
}

// ----------------------------------------------------------------------------

physaddr_t bootReserve(size_t length, size_t alignment) {
	assert(length <= pageSize);
	assert(alignment <= pageSize);

	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor accessor{
		    regions[i].address, pageShift, table, regions[i].numRoots, regions[i].order
		};
		auto physical = accessor.allocate(0, sizeof(uintptr_t) * CHAR_BIT);
		if (physical == BuddyAccessor::illegalAddress)
			continue;
		return physical;
	}

	eir::panicLogger() << "Eir: Out of memory" << frg::endlog;
	__builtin_unreachable();
}

physaddr_t allocPage() {
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor accessor{
		    regions[i].address, pageShift, table, regions[i].numRoots, regions[i].order
		};
		auto physical = accessor.allocate(0, sizeof(uintptr_t) * CHAR_BIT);
		if (physical == BuddyAccessor::illegalAddress)
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

address_t kasanToShadow(address_t address) { return kasanShadowDelta + (address >> kasanShift); }

void setShadowRange(address_t base, size_t size, int8_t value) {
	assert(!(base & (kasanScale - 1)));
	assert(!(size & (kasanScale - 1)));

	size_t progress = 0;
	while (progress < size) {
		auto shadow = kasanToShadow(base + progress);
		auto page = shadow & ~address_t{pageSize - 1};
		auto physical = getSingle4kPage(page);
		assert(physical != static_cast<address_t>(-1));

		auto p = physToVirt<int8_t>(physical);
		auto n = shadow & (pageSize - 1);
		while (n < pageSize && progress < size) {
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

	auto p = physToVirt<int8_t>(physical);
	auto n = shadow & (pageSize - 1);
	assert(p[n] == static_cast<int8_t>(0xFF));
	p[n] = value;
};
} // namespace
#endif // EIR_KASAN

void mapKasanShadow(address_t base, size_t size) {
#ifdef EIR_KASAN
	assert(!(base & (kasanScale - 1)));

	eir::infoLogger() << "eir: Mapping KASAN shadow for 0x" << frg::hex_fmt{base} << ", size: 0x"
	                  << frg::hex_fmt{size} << frg::endlog;

	size = (size + kasanScale - 1) & ~(kasanScale - 1);

	for (address_t page = (kasanToShadow(base) & ~address_t{pageSize - 1});
	     page < ((kasanToShadow(base + size) + pageSize - 1) & ~address_t{pageSize - 1});
	     page += pageSize) {
		auto physical = getSingle4kPage(page);
		if (physical != static_cast<address_t>(-1))
			continue;
		physical = allocPage();
		memset(physToVirt<uint8_t>(physical), 0xFF, pageSize);
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
	if (size & (kasanScale - 1))
		setShadowByte(base + (size & ~(kasanScale - 1)), size & (kasanScale - 1));
#else
	(void)base;
	(void)size;
#endif // EIR_KASAN
}

// ----------------------------------------------------------------------------

void mapRegionsAndStructs() {
	const auto &ml = getMemoryLayout();

	// This region should be available RAM on every PC.
	for (size_t page = 0x8000; page < 0x80000; page += pageSize) {
		mapSingle4kPage(ml.directPhysical + page, page, PageFlags::write | PageFlags::global);
		mapSingle4kPage(page, page, PageFlags::write | PageFlags::global | PageFlags::execute);
	}

	mapKasanShadow(ml.directPhysical + 0x8000, 0x80000);
	unpoisonKasanShadow(ml.directPhysical + 0x8000, 0x80000);

	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		// Map the region itself.
		for (address_t page = 0; page < regions[i].size; page += pageSize)
			mapSingle4kPage(
			    ml.directPhysical + regions[i].address + page,
			    regions[i].address + page,
			    PageFlags::write | PageFlags::global
			);
		mapKasanShadow(ml.directPhysical + regions[i].address, regions[i].size);
		unpoisonKasanShadow(ml.directPhysical + regions[i].address, regions[i].size);

		// Map the buddy tree (also to the direct physical map).
		address_t buddyMapping = ml.directPhysical + regions[i].buddyTree;
		for (address_t page = 0; page < regions[i].buddyOverhead; page += pageSize) {
			mapSingle4kPage(
			    buddyMapping + page,
			    regions[i].buddyTree + page,
			    PageFlags::write | PageFlags::global
			);
		}
		mapKasanShadow(buddyMapping, regions[i].buddyOverhead);
		unpoisonKasanShadow(buddyMapping, regions[i].buddyOverhead);
		regions[i].buddyMap = buddyMapping;
	}
}

void allocLogRingBuffer() {
	const auto &ml = getMemoryLayout();
	for (size_t i = 0; i < ml.allocLogSize; i += pageSize)
		mapSingle4kPage(ml.allocLog + i, allocPage(), PageFlags::write | PageFlags::global);
	mapKasanShadow(ml.allocLog, ml.allocLogSize);
	unpoisonKasanShadow(ml.allocLog, ml.allocLogSize);
}

// ----------------------------------------------------------------------------
// Bootstrap information handling.
// ----------------------------------------------------------------------------

address_t bootstrapDataPointer = 0;

address_t mapBootstrapData(void *p) {
	if (!bootstrapDataPointer)
		bootstrapDataPointer = getMemoryLayout().eirInfo;

	auto pointer = bootstrapDataPointer;
	bootstrapDataPointer += pageSize;
	mapSingle4kPage(pointer, virtToPhys(p), 0);
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
	    reinterpret_cast<uint8_t *>(initrd), initrd_end - reinterpret_cast<uintptr_t>(initrd)
	};

	for (auto entry : cpio_range) {
		if (entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	if (!kernel_image.data() || !kernel_image.size())
		eir::panicLogger() << "eir: could not find thor in the initrd.cpio" << frg::endlog;
}

namespace {

bool patchGenericManagarmElfNote(unsigned int type, frg::span<char> desc) {
	if (type == elf_note_type::memoryLayout) {
		if (desc.size() != sizeof(MemoryLayout))
			panicLogger() << "MemoryLayout size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &getMemoryLayout(), sizeof(MemoryLayout));
		return true;
	} else if (type == elf_note_type::perCpuRegion) {
		if (desc.size() != sizeof(PerCpuRegion))
			panicLogger() << "PerCpuRegion size does not match ELF note" << frg::endlog;
		memcpy(&perCpuRegion, desc.data(), sizeof(PerCpuRegion));
		return true;
	} else if (type == elf_note_type::smbiosData) {
		if (desc.size() != sizeof(SmbiosData))
			panicLogger() << "SmbiosData size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &eirSmbios3Addr, sizeof(SmbiosData));
		return true;
	} else if (type == elf_note_type::bootUartConfig) {
		if (desc.size() != sizeof(BootUartConfig))
			panicLogger() << "BootUartConfig size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &uart::bootUartConfig, sizeof(BootUartConfig));
		return true;
	}
	return false;
}

} // namespace

void loadKernelImage(void *imagePtr) {
	auto image = reinterpret_cast<char *>(imagePtr);

	Elf64_Ehdr ehdr;
	memcpy(&ehdr, image, sizeof(Elf64_Ehdr));
	if (ehdr.e_ident[0] != '\x7F' || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L'
	    || ehdr.e_ident[3] != 'F') {
		eir::panicLogger() << "Illegal magic fields" << frg::endlog;
	}
	assert(ehdr.e_type == ET_EXEC);

	// Read and patch Thor's ELF notes.
	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(&phdr, image + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(Elf64_Phdr));
		if (phdr.p_type != PT_NOTE)
			continue;
		if (phdr.p_memsz != phdr.p_filesz)
			panicLogger() << "Eir does not support p_filesz != p_memsz for PT_NOTE" << frg::endlog;
		size_t offset = 0;
		while (offset < phdr.p_memsz) {
			Elf64_Nhdr nhdr;
			memcpy(&nhdr, image + phdr.p_offset + offset, sizeof(Elf64_Nhdr));
			offset += sizeof(Elf64_Nhdr);

			auto *namePtr = image + phdr.p_offset + offset;
			offset += nhdr.n_namesz + 1;
			offset = (offset + 7) & ~size_t{7};
			auto *descPtr = image + phdr.p_offset + offset;
			offset += nhdr.n_descsz;
			offset = (offset + 7) & ~size_t{7};

			frg::string_view name{namePtr, nhdr.n_namesz};
			frg::span<char> desc{descPtr, nhdr.n_descsz};
			infoLogger() << "ELF note: " << name << ", type 0x" << frg::hex_fmt{nhdr.n_type}
			             << frg::endlog;
			if (name != "Managarm")
				continue;
			if (elf_note_type::isThorGeneric(nhdr.n_type)) {
				if (!patchGenericManagarmElfNote(nhdr.n_type, desc))
					panicLogger() << "Failed to patch generic Managarm ELF note"
					              << " with type 0x" << frg::hex_fmt{nhdr.n_type} << frg::endlog;
			} else if (elf_note_type::isThorArchSpecific(nhdr.n_type)) {
				if (!patchArchSpecificManagarmElfNote(nhdr.n_type, desc))
					panicLogger() << "Failed to patch arch-specific Managarm ELF note"
					              << " with type 0x" << frg::hex_fmt{nhdr.n_type} << frg::endlog;
			} else {
				panicLogger() << "Managarm ELF note type 0x" << frg::hex_fmt{nhdr.n_type}
				              << " is not within known range" << frg::endlog;
			}
		}
	}

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(
		    &phdr,
		    (void *)((uintptr_t)image + (uintptr_t)ehdr.e_phoff + i * ehdr.e_phentsize),
		    sizeof(Elf64_Phdr)
		);
		if (phdr.p_type != PT_LOAD)
			continue;
		assert(!(phdr.p_offset & (pageSize - 1)));
		assert(!(phdr.p_vaddr & (pageSize - 1)));

		uint32_t map_flags = PageFlags::global;
		if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
			// no additional flags
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= PageFlags::write;
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= PageFlags::execute;
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W | PF_X)) {
			eir::infoLogger() << "eir: warning: Mapping PHDR with RWX permissions" << frg::endlog;
			map_flags |= PageFlags::write | PageFlags::execute;
		} else {
			eir::panicLogger() << "Illegal combination of segment permissions" << frg::endlog;
		}

		uintptr_t pg = 0;
		while (pg < (uintptr_t)phdr.p_memsz) {
			auto backing = allocPage();
			auto backingVirt = physToVirt<uint8_t>(backing);
			memset(backingVirt, 0, pageSize);
			if (pg < (uintptr_t)phdr.p_filesz)
				memcpy(
				    backingVirt,
				    reinterpret_cast<void *>((uintptr_t)image + (uintptr_t)phdr.p_offset + pg),
				    frg::min(pageSize, (uintptr_t)phdr.p_filesz - pg)
				);
			mapSingle4kPage(phdr.p_vaddr + pg, backing, map_flags);
			pg += pageSize;
		}
		mapKasanShadow(phdr.p_paddr, phdr.p_memsz);
		unpoisonKasanShadow(phdr.p_paddr, phdr.p_memsz);
	}

	// Map the KASAN shadow for thor's per-CPU regions.
	{
		assert(perCpuRegion.start && perCpuRegion.end);

		// TODO(qookie): Figure out the number of cores
		// instead of mapping shadow for 256.
		int nrCores = 256;

		auto singleSize = perCpuRegion.end - perCpuRegion.start;
		assert(!(singleSize & 0xFFF));

		// The BSPs region is covered by a PT_LOAD PHDR already.
		auto totalSize = singleSize * (nrCores - 1);

		mapKasanShadow(perCpuRegion.start + singleSize, totalSize);

		// We don't call unpoisonKasanShadow here as thor will
		// unpoison these regions as it allocates them.
	}

	kernelEntry = ehdr.e_entry;
}

namespace {

void generateInfo() {
	// Setup the eir interface struct.
	auto info_ptr = bootAlloc<EirInfo>();
	memset(info_ptr, 0, sizeof(EirInfo));
	auto info_vaddr = mapBootstrapData(info_ptr);
	assert(info_vaddr == getMemoryLayout().eirInfo);
	info_ptr->signature = eirSignatureValue;

	// Pass firmware tables.
	if (eirRsdpAddr) {
		info_ptr->acpiRsdp = eirRsdpAddr;
	}
	if (eirDtbPtr) {
		DeviceTree dt{physToVirt<void>(eirDtbPtr)};
		info_ptr->dtbPtr = eirDtbPtr;
		info_ptr->dtbSize = dt.size();
	}

#ifdef __riscv
	info_ptr->hartId = eirBootHartId;
#endif

	// Pass all memory regions to thor.
	int n = 0;
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType == RegionType::allocatable)
			n++;
	}

	auto regionInfos = bootAlloc<EirRegion>(n);
	info_ptr->numRegions = n;
	info_ptr->regionInfo = mapBootstrapData(regionInfos);
	int j = 0;
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		regionInfos[j].address = regions[i].address;
		regionInfos[j].length = regions[i].size;
		regionInfos[j].order = regions[i].order;
		regionInfos[j].numRoots = regions[i].numRoots;
		regionInfos[j].buddyTree = regions[i].buddyMap;
		j++;
	}

	// Parse the kernel command line.
	bool serial{false};
	bool bochs{false};
	bool kernelProfile{false};
	frg::array options = {
	    frg::option{"serial", frg::store_true(serial)},
	    frg::option{"bochs", frg::store_true(bochs)},
	    frg::option{"kernel-profile", frg::store_true(kernelProfile)},
	};
	parseCmdline(options);

	if (serial)
		info_ptr->debugFlags |= eirDebugSerial;
	if (bochs)
		info_ptr->debugFlags |= eirDebugBochs;
	if (kernelProfile)
		info_ptr->debugFlags |= eirDebugKernelProfile;

	// Pass the command line to Thor.
	auto cmdlineChunks = getCmdline();

	// For each chunk: we either have a trailing space or null terminator.
	auto cmdlineLength = cmdlineChunks.size();
	for (auto chunk : cmdlineChunks)
		cmdlineLength += chunk.size();

	if (cmdlineLength > pageSize)
		panicLogger() << "eir: Command line exceeds page size" << frg::endlog;
	auto cmdlineBuffer = bootAlloc<char>(cmdlineLength);

	char *cmdlinePtr = cmdlineBuffer;
	for (auto chunk : cmdlineChunks) {
		if (!chunk.size())
			continue;
		if (cmdlinePtr != cmdlineBuffer)
			*(cmdlinePtr++) = ' ';
		memcpy(cmdlinePtr, chunk.data(), chunk.size());
		cmdlinePtr += chunk.size();
	}
	*cmdlinePtr = '\0';

	infoLogger() << "eir: Kernel command line: '" << cmdlineBuffer << "'" << frg::endlog;

	info_ptr->commandLine = mapBootstrapData(cmdlineBuffer);

	auto initrd_module = bootAlloc<EirModule>(1);
	initrd_module->physicalBase = virtToPhys(initrd);
	initrd_module->length = initrd_image.size();
	const char *initrd_mod_name = "initrd.cpio";
	size_t name_length = strlen(initrd_mod_name);
	char *name_ptr = bootAlloc<char>(name_length);
	memcpy(name_ptr, initrd_mod_name, name_length);
	initrd_module->namePtr = mapBootstrapData(name_ptr);
	initrd_module->nameLength = name_length;

	info_ptr->moduleInfo = mapBootstrapData(initrd_module);

	// Pass the framebuffer to thor.
	auto *fb = getFramebuffer();
	if (fb) {
		info_ptr->frameBuffer = *fb;
		info_ptr->frameBuffer.fbEarlyWindow = getKernelFrameBuffer();
	}
}

static initgraph::Task generateInfoStruct{
    &globalInitEngine,
    "generic.generate-thor-info-struct",
    initgraph::Requires{
        getInitrdAvailableStage(), getCmdlineAvailableStage(), getKernelLoadableStage()
    },
    [] { generateInfo(); }
};

} // namespace

} // namespace eir
