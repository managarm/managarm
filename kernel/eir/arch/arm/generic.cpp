#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <elf.h>

namespace eir {

#if PIE

extern "C" [[gnu::visibility("hidden")]] Elf64_Dyn _DYNAMIC[];

void eirRelocate() {
	auto base = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t relaAddr = 0;
	uintptr_t relaSize = 0;

	for (auto *dyn = _DYNAMIC; dyn->d_tag != DT_NULL; ++dyn) {
		switch (dyn->d_tag) {
			case DT_RELA:
				relaAddr = dyn->d_ptr + base;
				break;
			case DT_RELASZ:
				relaSize = dyn->d_val;
				break;
			default:
				break;
		}
	}

	for (uintptr_t i = 0; i < relaSize; i += sizeof(Elf64_Rela)) {
		auto *rela = reinterpret_cast<const Elf64_Rela *>(relaAddr + i);
		assert(ELF64_R_TYPE(rela->r_info) == R_AARCH64_RELATIVE);
		*reinterpret_cast<Elf64_Addr *>(rela->r_offset + base) = base + rela->r_addend;
	}
}

#endif // PIE

[[noreturn]] void eirGenericMain(const GenericInfo &genericInfo) {
	initProcessorEarly();

	DeviceTree dt{reinterpret_cast<void *>(genericInfo.deviceTreePtr)};

	eir::infoLogger() << "DTB pointer " << dt.data() << frg::endlog;
	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dt.size()} << frg::endlog;

	DeviceTreeNode chosenNode;
	bool hasChosenNode = false;

	DeviceTreeNode reservedMemoryNode;
	bool hasReservedMemoryNode = false;

	DeviceTreeNode memoryNodes[32];
	size_t nMemoryNodes = 0;

	dt.rootNode().discoverSubnodes(
	    [](DeviceTreeNode &node) {
		    return !memcmp("memory", node.name(), 6) || !memcmp("chosen", node.name(), 7)
		           || !memcmp("reserved-memory", node.name(), 15);
	    },
	    [&](DeviceTreeNode node) {
		    if (!memcmp("chosen", node.name(), 7)) {
			    assert(!hasChosenNode);

			    chosenNode = node;
			    hasChosenNode = true;
		    } else if (!memcmp("reserved-memory", node.name(), 15)) {
			    assert(!hasReservedMemoryNode);

			    reservedMemoryNode = node;
			    hasReservedMemoryNode = true;
		    } else {
			    assert(nMemoryNodes < 32);

			    memoryNodes[nMemoryNodes++] = node;
		    }
		    infoLogger() << "Node \"" << node.name() << "\" discovered" << frg::endlog;
	    }
	);

	uint32_t addressCells = 2, sizeCells = 1;

	for (auto prop : dt.rootNode().properties()) {
		if (!memcmp("#address-cells", prop.name(), 15)) {
			addressCells = prop.asU32();
		} else if (!memcmp("#size-cells", prop.name(), 12)) {
			sizeCells = prop.asU32();
		}
	}

	assert(nMemoryNodes && hasChosenNode);

	InitialRegion reservedRegions[64];
	size_t nReservedRegions = 0;

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;

	auto addReservedRegion = [&](address_t base, address_t size) {
		assert(nReservedRegions < sizeof(reservedRegions) / sizeof(*reservedRegions));
		reservedRegions[nReservedRegions++] = {base, size};
	};

	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "At 0x" << frg::hex_fmt{ent.address} << ", ends at 0x"
		                  << frg::hex_fmt{ent.address + ent.size} << " (0x"
		                  << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;
		addReservedRegion(ent.address, ent.size);
	}

	if (hasReservedMemoryNode) {
		// reserved-memory should have same address-cells and size-cells as the root node.
		for (auto prop : reservedMemoryNode.properties()) {
			if (!memcmp("#address-cells", prop.name(), 15)) {
				assert(prop.asU32() == addressCells);
			} else if (!memcmp("#size-cells", prop.name(), 12)) {
				assert(prop.asU32() == sizeCells);
			}
		}

		reservedMemoryNode.discoverSubnodes(
		    [](DeviceTreeNode &) { return true; },
		    [&](DeviceTreeNode node) {
			    auto reg = node.findProperty("reg");
			    if (!reg) {
				    return;
			    }

			    size_t i = 0;
			    while (i < reg->size()) {
				    auto base = reg->asPropArrayEntry(addressCells, i);
				    i += addressCells * 4;

				    auto size = reg->asPropArrayEntry(sizeCells, i);
				    i += sizeCells * 4;

				    eir::infoLogger() << "At 0x" << frg::hex_fmt{base} << ", ends at 0x"
				                      << frg::hex_fmt{base + size} << " (0x" << frg::hex_fmt{size}
				                      << " bytes)" << frg::endlog;
				    addReservedRegion(base, size);
			    }
		    }
		);
	}

	eir::infoLogger() << "End of memory reservation entries" << frg::endlog;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	addReservedRegion(eirStart, eirEnd - eirStart);

	uintptr_t initrd = 0;
	if (auto p = chosenNode.findProperty("linux,initrd-start"); p) {
		if (p->size() == 4)
			initrd = p->asU32();
		else if (p->size() == 8)
			initrd = p->asU64();
		else
			assert(!"Invalid linux,initrd-start size");

		eir::infoLogger() << "Initrd is at " << (void *)initrd << frg::endlog;
	} else {
		initrd = 0x48000000;
		eir::infoLogger() << "Assuming initrd is at " << (void *)initrd << frg::endlog;
	}

	parseInitrd(reinterpret_cast<void *>(initrd));

	addReservedRegion(initrd, initrd_image.size());
	addReservedRegion(genericInfo.deviceTreePtr, dt.size());

	for (size_t i = 0; i < nMemoryNodes; i++) {
		auto reg = memoryNodes[i].findProperty("reg");
		assert(reg);

		size_t j = 0;
		while (j < reg->size()) {
			auto base = reg->asPropArrayEntry(addressCells, j);
			j += addressCells * 4;

			auto size = reg->asPropArrayEntry(sizeCells, j);
			j += sizeCells * 4;

			createInitialRegions({base, size}, {reservedRegions, nReservedRegions});
		}
	}

	setupRegionStructs();

	eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
	for (size_t i = 0; i < numRegions; ++i) {
		if (regions[i].regionType == RegionType::null)
			continue;
		eir::infoLogger() << "    Memory region [" << i << "]."
		                  << " Base: 0x" << frg::hex_fmt{regions[i].address} << ", length: 0x"
		                  << frg::hex_fmt{regions[i].size} << frg::endlog;
		if (regions[i].regionType == RegionType::allocatable)
			eir::infoLogger() << "        Buddy tree at 0x" << frg::hex_fmt{regions[i].buddyTree}
			                  << ", overhead: 0x" << frg::hex_fmt{regions[i].buddyOverhead}
			                  << frg::endlog;
	}

	uint64_t kernel_entry = 0;
	initProcessorPaging(kernel_image.data(), kernel_entry);

	const char *cmdline = "";
	if (genericInfo.cmdline) {
		cmdline = genericInfo.cmdline;
	} else if (auto p = chosenNode.findProperty("bootargs"); p) {
		cmdline = static_cast<const char *>(p->data());
	}

	auto info_ptr = generateInfo(cmdline);

	auto module = bootAlloc<EirModule>();
	module->physicalBase = initrd;
	module->length = initrd_image.size();

	char *name_ptr = bootAlloc<char>(11);
	memcpy(name_ptr, "initrd.cpio", 11);
	module->namePtr = mapBootstrapData(name_ptr);
	module->nameLength = 11;

	info_ptr->moduleInfo = mapBootstrapData(module);

	info_ptr->dtbPtr = genericInfo.deviceTreePtr;
	info_ptr->dtbSize = dt.size();

	if (genericInfo.hasFb) {
		info_ptr->frameBuffer = genericInfo.fb;
		assert(genericInfo.fb.fbAddress & ~(pageSize - 1));
		size_t fbSize = genericInfo.fb.fbPitch * genericInfo.fb.fbHeight;

		for (address_t pg = 0; pg < fbSize; pg += 0x1000)
			mapSingle4kPage(
			    0xFFFF'FE00'4000'0000 + pg,
			    genericInfo.fb.fbAddress + pg,
			    PageFlags::write,
			    CachingMode::writeCombine
			);

		mapKasanShadow(0xFFFF'FE00'4000'0000, fbSize);
		unpoisonKasanShadow(0xFFFF'FE00'4000'0000, fbSize);
		info_ptr->frameBuffer.fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	}

	info_ptr->debugFlags = genericInfo.debugFlags;

	mapSingle4kPage(0xFFFF'0000'0000'0000, 0x9000000, PageFlags::write, CachingMode::mmio);
	mapKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
	unpoisonKasanShadow(0xFFFF'0000'0000'0000, 0x1000);

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;

	eirEnterKernel(eirTTBR[0] + 1, eirTTBR[1] + 1, kernel_entry, 0xFFFF'FE80'0001'0000);

	while (true) {
		asm volatile("" : : : "memory");
	}
}

} // namespace eir
