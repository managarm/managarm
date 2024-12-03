#include <assert.h>
#include <dtb.hpp>

#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <frg/array.hpp>

namespace eir {

size_t findSizeCells(DeviceTreeNode parent) {
	auto maybeProperty = parent.findProperty("#address-cells");
	if (maybeProperty)
		return maybeProperty->asU32();
	return 2;
}

size_t findAddressCells(DeviceTreeNode parent) {
	auto maybeProperty = parent.findProperty("#size-cells");
	if (maybeProperty)
		return maybeProperty->asU32();
	return 1;
}

void discoverMemoryFromDtb(void *dtbPtr) {
	eir::infoLogger() << "DTB pointer " << dtbPtr << frg::endlog;

	DeviceTree dt{dtbPtr};
	auto rootNode = dt.rootNode();

	// Debugging code to dump the DTB.
	struct Walker {
		void push(DeviceTreeNode node) {
			auto log = eir::infoLogger();
			for (unsigned int i = 0; i < nesting_; ++i)
				log << "  ";
			log << node.name() << frg::endlog;

			++nesting_;

			for (auto prop : node.properties()) {
				auto plog = eir::infoLogger();
				for (unsigned int i = 0; i < nesting_; ++i)
					plog << "  ";
				plog << "Property " << prop.name() << ", " << prop.size() << " bytes"
				     << frg::endlog;
			}
		}

		void pop() { --nesting_; }

	private:
		unsigned int nesting_ = 0;
	} w;

	eir::infoLogger() << "Dumping DTB" << frg::endlog;
	dt.walkTree(w);

	// Find the initrd.
	DeviceTreeNode chosenNode;
	bool foundChosen = false;

	dt.rootNode().discoverSubnodes(
	    [](DeviceTreeNode &node) {
		    auto name = frg::string_view(node.name());
		    return name == "chosen";
	    },
	    [&](DeviceTreeNode node) {
		    chosenNode = node;
		    foundChosen = true;
	    }
	);

	if (!foundChosen)
		eir::panicLogger() << "DTB does not contain \"chosen\" node" << frg::endlog;

	uint64_t initrdStart = 0;
	uint64_t initrdEnd = 0;
	if (auto p = chosenNode.findProperty("linux,initrd-start"); p) {
		if (p->size() == 4)
			initrdStart = p->asU32();
		else if (p->size() == 8)
			initrdStart = p->asU64();
		else
			eir::panicLogger() << "Invalid linux,initrd-start size" << frg::endlog;
	}
	if (auto p = chosenNode.findProperty("linux,initrd-end"); p) {
		if (p->size() == 4)
			initrdEnd = p->asU32();
		else if (p->size() == 8)
			initrdEnd = p->asU64();
		else
			eir::panicLogger() << "Invalid linux,initrd-end size" << frg::endlog;
	}

	eir::infoLogger() << "initrd is between 0x" << frg::hex_fmt(initrdStart) << " and 0x"
	                  << frg::hex_fmt(initrdEnd) << frg::endlog;

	// Determine all reserved memory areas.
	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {eirStart, eirEnd - eirStart};
	reservedRegions[nReservedRegions++] = {initrdStart, initrdEnd - initrdStart};
	reservedRegions[nReservedRegions++] = {reinterpret_cast<uintptr_t>(dtbPtr), dt.size()};

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;

	// Handle entries from the top-level table within the DTB.
	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "    At 0x" << frg::hex_fmt{ent.address} << ", ends at 0x"
		                  << frg::hex_fmt{ent.address + ent.size} << " (0x"
		                  << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;

		if (nReservedRegions >= 32)
			eir::panicLogger() << "Cannot deal with > 32 DTB memory reservations" << frg::endlog;
		reservedRegions[nReservedRegions++] = {ent.address, ent.size};
	}

	// Handle entries from /reserved-memory.
	dt.rootNode().discoverSubnodes(
	    [](DeviceTreeNode &node) {
		    auto name = frg::string_view(node.name());
		    return name == "reserved-memory";
	    },
	    [&](DeviceTreeNode reservedNode) {
		    auto addressCells = findAddressCells(reservedNode);
		    auto sizeCells = findSizeCells(reservedNode);

		    reservedNode.discoverSubnodes(
		        [](DeviceTreeNode &) { return true; },
		        [&](DeviceTreeNode childNode) {
			        // Children without "reg" correspond to OS-allocated reserved memory.
			        auto reg = childNode.findProperty("reg");
			        if (!reg) {
				        eir::infoLogger() << "DTB reserved-memory child " << childNode.name()
				                          << " has no \"reg\" property" << frg::endlog;
				        return;
			        }

			        size_t j = 0;
			        while (j < reg->size()) {
				        auto base = reg->asPropArrayEntry(addressCells, j);
				        j += addressCells * 4;

				        auto size = reg->asPropArrayEntry(sizeCells, j);
				        j += sizeCells * 4;

				        eir::infoLogger()
				            << "    " << childNode.name() << ", at 0x" << frg::hex_fmt{base}
				            << ", ends at 0x" << frg::hex_fmt{base + size} << " (0x"
				            << frg::hex_fmt{size} << " bytes)" << frg::endlog;
				        reservedRegions[nReservedRegions++] = {base, size};
			        }
		        }
		    );
	    }
	);

	// Find all memory nodes.
	dt.rootNode().discoverSubnodes(
	    [](DeviceTreeNode &node) { return !memcmp("memory@", node.name(), 7); },
	    [&](DeviceTreeNode node) {
		    auto addressCells = findAddressCells(rootNode);
		    auto sizeCells = findSizeCells(rootNode);

		    auto reg = node.findProperty("reg");
		    if (!reg)
			    eir::panicLogger() << "DTB memory node has no \"reg\" property" << frg::endlog;

		    size_t j = 0;
		    while (j < reg->size()) {
			    auto base = reg->asPropArrayEntry(addressCells, j);
			    j += addressCells * 4;

			    auto size = reg->asPropArrayEntry(sizeCells, j);
			    j += sizeCells * 4;

			    createInitialRegions({base, size}, {reservedRegions, nReservedRegions});
		    }
	    }
	);

	// Create and dump the resulting memory regions.
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
}

} // namespace eir
