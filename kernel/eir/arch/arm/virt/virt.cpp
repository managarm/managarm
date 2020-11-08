#include <stdint.h>
#include <assert.h>
#include <eir-internal/debug.hpp>
#include <eir/interface.hpp>
#include <render-text.hpp>
#include <dtb.hpp>
#include "../cpio.hpp"
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/tuple.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

#include <arch/bit.hpp>
#include <arch/variable.hpp>

namespace eir {

namespace PL011 {
	namespace reg {
		static constexpr arch::scalar_register<uint32_t> data{0x00};
		static constexpr arch::bit_register<uint32_t> status{0x18};
		static constexpr arch::scalar_register<uint32_t> i_baud{0x24};
		static constexpr arch::scalar_register<uint32_t> f_baud{0x28};
		static constexpr arch::bit_register<uint32_t> control{0x30};
		static constexpr arch::bit_register<uint32_t> line_control{0x2c};
		static constexpr arch::scalar_register<uint32_t> int_clear{0x44};
	}

	namespace status {
		static constexpr arch::field<uint32_t, bool> tx_full{5, 1};
	};

	namespace control {
		static constexpr arch::field<uint32_t, bool> rx_en{9, 1};
		static constexpr arch::field<uint32_t, bool> tx_en{8, 1};
		static constexpr arch::field<uint32_t, bool> uart_en{0, 1};
	};

	namespace line_control {
		static constexpr arch::field<uint32_t, uint8_t> word_len{5, 2};
		static constexpr arch::field<uint32_t, bool> fifo_en{4, 1};
	}

	static constexpr arch::mem_space space{0x9000000};
	constexpr uint64_t clock = 24000000; // 24MHz

	void init(uint64_t baud) {
		space.store(reg::control, control::uart_en(false));

		space.store(reg::int_clear, 0x7FF); // clear all interrupts

		uint64_t int_part = clock / (16 * baud);

		// 3 decimal places of precision should be enough :^)
		uint64_t frac_part = (((clock * 1000) / (16 * baud) - (int_part * 1000))
			* 64 + 500) / 1000;

		space.store(reg::i_baud, int_part);
		space.store(reg::f_baud, frac_part);

		// 8n1, fifo enabled
		space.store(reg::line_control, line_control::word_len(3) | line_control::fifo_en(true));
		space.store(reg::control, control::rx_en(true) | control::tx_en(true) | control::uart_en(true));
	}

	void send(uint8_t val) {
		while (space.load(reg::status) & status::tx_full)
			;

		space.store(reg::data, val);
	}
}

void debugPrintChar(char c) {
	PL011::send(c);
}

extern "C" void eirEnterKernel(uintptr_t, uintptr_t, uint64_t, uint64_t, uintptr_t);

extern "C" void eirVirtMain(uintptr_t deviceTreePtr) {
	PL011::init(115200);

	initProcessorEarly();

	DeviceTree dt{reinterpret_cast<void *>(deviceTreePtr)};

	eir::infoLogger() << "DTB pointer " << dt.data() << frg::endlog;
	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dt.size()} << frg::endlog;

	DeviceTreeNode chosenNode;
	bool hasChosenNode = false;

	DeviceTreeNode memoryNodes[32];
	size_t nMemoryNodes = 0;

	dt.rootNode().discoverSubnodes(
		[](DeviceTreeNode &node) {
			return !memcmp("memory@", node.name(), 7)
				|| !memcmp("chosen", node.name(), 7);
		},
		[&](DeviceTreeNode node) {
			if (!memcmp("chosen", node.name(), 7)) {
				assert(!hasChosenNode);

				chosenNode = node;
				hasChosenNode = true;
			} else {
				assert(nMemoryNodes < 32);

				memoryNodes[nMemoryNodes++] = node;
			}
			infoLogger() << "Node \"" << node.name() << "\" discovered" << frg::endlog;
		});

	uint32_t addressCells = 2, sizeCells = 1;

	for (auto prop : dt.rootNode().properties()) {
		if (!memcmp("#address-cells", prop.name(), 15)) {
			addressCells = prop.asU32();
		} else if (!memcmp("#size-cells", prop.name(), 12)) {
			sizeCells = prop.asU32();
		}
	}

	assert(nMemoryNodes && hasChosenNode);

	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;
	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "At 0x" << frg::hex_fmt{ent.address}
			<< ", ends at 0x" << frg::hex_fmt{ent.address + ent.size}
			<< " (0x" << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;

		reservedRegions[nReservedRegions++] = {ent.address, ent.size};
	}
	eir::infoLogger() << "End of memory reservation entries" << frg::endlog;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {eirStart, eirEnd - eirStart};

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

	CpioRange cpio_range{reinterpret_cast<void *>(initrd)};

	auto initrd_end = reinterpret_cast<uintptr_t>(cpio_range.eof());
	eir::infoLogger() << "Initrd ends at " << (void *)initrd_end << frg::endlog;

	reservedRegions[nReservedRegions++] = {initrd, initrd_end - initrd};
	reservedRegions[nReservedRegions++] = {deviceTreePtr, dt.size()};

	for (int i = 0; i < nMemoryNodes; i++) {
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
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		eir::infoLogger() << "    Memory region [" << i << "]."
				<< " Base: 0x" << frg::hex_fmt{regions[i].address}
				<< ", length: 0x" << frg::hex_fmt{regions[i].size} << frg::endlog;
		if(regions[i].regionType == RegionType::allocatable)
			eir::infoLogger() << "        Buddy tree at 0x" << frg::hex_fmt{regions[i].buddyTree}
					<< ", overhead: 0x" << frg::hex_fmt{regions[i].buddyOverhead}
					<< frg::endlog;
	}

	frg::span<uint8_t> kernel_image{nullptr, 0};

	for (auto entry : cpio_range) {
		if (entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	assert(kernel_image.data() && kernel_image.size());

	uint64_t kernel_entry = 0;
	initProcessorPaging(kernel_image.data(), kernel_entry);

	auto info_ptr = generateInfo("");

	auto module = bootAlloc<EirModule>();
	module->physicalBase = initrd;
	module->length = initrd_end - initrd;

	char *name_ptr = bootAlloc<char>(11);
	memcpy(name_ptr, "initrd.cpio", 11);
	module->namePtr = mapBootstrapData(name_ptr);
	module->nameLength = 11;

	info_ptr->numModules = 1;
	info_ptr->moduleInfo = mapBootstrapData(module);

	info_ptr->debugFlags |= eirDebugSerial;

	mapSingle4kPage(0xFFFF'0000'0000'0000, 0x9000000,
			PageFlags::write, CachingMode::mmio);
	mapKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
	unpoisonKasanShadow(0xFFFF'0000'0000'0000, 0x1000);

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;

	eirEnterKernel(eirTTBR[0] + 1, eirTTBR[1] + 1, kernel_entry,
			0xFFFF'FE80'0001'0000, 0xFFFF'FE80'0001'0000);

	while(true);
}

enum class IntrType {
	synchronous,
	irq,
	fiq,
	serror
};

extern "C" void eirExceptionHandler(IntrType i_type, uintptr_t syndrome, uintptr_t link,
		uintptr_t state, uintptr_t fault_addr) {

	// Disable MMU to gain back the ability to use the screen and uart
	uint64_t sctlr;
	asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

	sctlr &= ~1;

	asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));

	eir::infoLogger() << "An unexpected fault has occured:" << frg::endlog;

	const char *i_type_str = "Unknown";
	switch (i_type) {
		case IntrType::synchronous:
			i_type_str = "synchronous";
			break;
		case IntrType::irq:
			i_type_str = "irq";
			break;
		case IntrType::fiq:
			i_type_str = "fiq";
			break;
		case IntrType::serror:
			i_type_str = "SError";
			break;
	}

	eir::infoLogger() << "Interruption type: " << i_type_str << frg::endlog;

	auto exc_type = syndrome >> 26;
	const char *exc_type_str = "Unknown";
	switch (exc_type) {
		case 0x01: exc_type_str = "Trapped WFI/WFE"; break;
		case 0x0e: exc_type_str = "Illegal execution"; break;
		case 0x15: exc_type_str = "System call"; break;
		case 0x20: exc_type_str = "Instruction abort, lower EL"; break;
		case 0x21: exc_type_str = "Instruction abort, same EL"; break;
		case 0x22: exc_type_str = "Instruction alignment fault"; break;
		case 0x24: exc_type_str = "Data abort, lower EL"; break;
		case 0x25: exc_type_str = "Data abort, same EL"; break;
		case 0x26: exc_type_str = "Stack alignment fault"; break;
		case 0x2c: exc_type_str = "Floating point"; break;
	}

	eir::infoLogger() << "Exception type: " << exc_type_str << " (" << (void *)exc_type << ")" << frg::endlog;

	auto iss = syndrome & ((1 << 25) - 1);

	if (exc_type == 0x25 || exc_type == 0x24) {
		constexpr const char *sas_values[4] = {"Byte", "Halfword", "Word", "Doubleword"};
		constexpr const char *set_values[4] = {"Recoverable", "Uncontainable", "Reserved", "Restartable/Corrected"};
		constexpr const char *dfsc_values[4] = {"Address size", "Translation", "Access flag", "Permission"};
		eir::infoLogger() << "Access size: " << sas_values[(iss >> 22) & 3] << frg::endlog;
		eir::infoLogger() << "Sign extended? " << (iss & (1 << 21) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Sixty-Four? " << (iss & (1 << 15) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Acquire/Release? " << (iss & (1 << 14) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Synch error type: " << set_values[(iss >> 11) & 3] << frg::endlog;
		eir::infoLogger() << "Fault address valid? " << (iss & (1 << 10) ? "No" : "Yes") << frg::endlog;
		eir::infoLogger() << "Cache maintenance? " << (iss & (1 << 8) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "S1PTW? " << (iss & (1 << 7) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Access type: " << (iss & (1 << 6) ? "Write" : "Read") << frg::endlog;
		if ((iss & 0b111111) <= 0b001111)
			eir::infoLogger() << "Data fault status code: " << dfsc_values[(iss >> 2) & 4] << " fault level " << (iss & 3) << frg::endlog;
		else if ((iss & 0b111111) == 0b10000)
			eir::infoLogger() << "Data fault status code: Synchronous external fault" << frg::endlog;
		else if ((iss & 0b111111) == 0b100001)
			eir::infoLogger() << "Data fault status code: Alignment fault" << frg::endlog;
		else if ((iss & 0b111111) == 0b110000)
			eir::infoLogger() << "Data fault status code: TLB conflict abort" << frg::endlog;
		else
			eir::infoLogger() << "Data fault status code: unknown" << frg::endlog;
	}

	eir::infoLogger() << "IP: " << (void *)link << ", State: " << (void *)state << frg::endlog;
	eir::infoLogger() << "Syndrome: " << (void *)syndrome << ", Fault address: " << (void *)fault_addr << frg::endlog;
	eir::infoLogger() << "Halting..." << frg::endlog;

	while(1)
		asm volatile ("wfi");
}

} // namespace eir
