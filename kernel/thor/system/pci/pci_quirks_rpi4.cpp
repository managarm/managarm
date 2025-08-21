#include <cstdint>
#include <frg/optional.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/pci/pci.hpp>

#ifdef THOR_HAS_DTB_SUPPORT
#include <thor-internal/dtb/dtb.hpp>
#endif // THOR_HAS_DTB_SUPPORT

#include <arch/mem_space.hpp>
#include <arch/cache.hpp>

namespace thor {

namespace {

namespace reg {
static constexpr arch::bit_register<uint32_t> read{0x00};
static constexpr arch::bit_register<uint32_t> status{0x18};
static constexpr arch::bit_register<uint32_t> write{0x20};
} // namespace reg

namespace io {
static constexpr arch::field<uint32_t, uint8_t> channel{0, 4};
static constexpr arch::field<uint32_t, uint32_t> value{4, 28};
} // namespace io

namespace status {
static constexpr arch::field<uint32_t, bool> empty{30, 1};
static constexpr arch::field<uint32_t, bool> full{31, 1};
} // namespace status

struct Bcm2835Mbox {
	Bcm2835Mbox(uintptr_t base) {
		auto va = KernelVirtualMemory::global().allocate(0x1000);

		auto offset = base & (kPageSize - 1);
		KernelPageSpace::global().mapSingle4k(
			reinterpret_cast<uintptr_t>(va),
			base & ~(kPageSize - 1),
			page_access::write,
			CachingMode::mmio);

		space_ = arch::mem_space{reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(va) + offset)};
		buf_ = physicalAllocator->allocate(kPageSize, 32);
		assert(buf_ != PhysicalAddr(-1) && "OOM");
	}

	void write(uint32_t value) {
		while (space_.load(reg::status) & status::full)
			;

		space_.store(reg::write, io::channel(8) | io::value(value >> 4));
	}

	uint32_t read() {
		while (space_.load(reg::status) & status::empty)
			;

		auto f = space_.load(reg::read);

		return (f & io::value) << 4;
	}

	void sendPropertyList(frg::span<uint32_t> data) {
		PageAccessor accessor{buf_};
		auto dataByteSize = data.size() * sizeof(uint32_t);

		uint32_t header[] = {
			uint32_t(dataByteSize) + 12,
			0, // RPI_FIRMWARE_STATUS_REQUEST
		};

		assert(dataByteSize + 12 <= kPageSize);

		memset(accessor.get(), 0, kPageSize);
		memcpy(accessor.get(), header, sizeof(header));
		memcpy(
			reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(accessor.get()) + sizeof(header)),
			data.data(), dataByteSize);
		// List of properties is terminated by a 0 word set by memset.
		arch::cache_writeback(reinterpret_cast<uintptr_t>(accessor.get()), kPageSize);

		write(buf_);
		auto out = read();

		assert(out == buf_);

		arch::cache_invalidate(reinterpret_cast<uintptr_t>(accessor.get()), kPageSize);
		uint32_t result;
		memcpy(
			&result,
			reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(accessor.get()) + sizeof(uint32_t)),
			sizeof(uint32_t));

		assert(result == 0x80000000); // RPI_FIRMWARE_STATUS_SUCCESS
	}

private:
	arch::mem_space space_;
	PhysicalAddr buf_;
};

static frg::manual_box<Bcm2835Mbox> globalMbox;

} // namespace anonymous


namespace pci {

// Definitely not on an RPi4.
#ifndef THOR_HAS_DTB_SUPPORT

void uploadRaspberryPi4Vl805Firmware(PciDevice *) {
	return;
}

#else

void uploadRaspberryPi4Vl805Firmware(PciDevice *dev) {
	// Not a VL805.
	if (dev->deviceId != 0x3483) return;

	DeviceTreeNode *rpiFwResetNode = nullptr;
	getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		if (node->isCompatible<1>({"raspberrypi,firmware-reset"})) {
			rpiFwResetNode = node;
			return true;
		}

		return false;
	});

	// Not on a RPi4.
	if (!rpiFwResetNode) return;

	DeviceTreeNode *rpiMboxNode = nullptr;
	getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		if (node->isCompatible<1>({"brcm,bcm2835-mbox"})) {
			rpiMboxNode = node;
			return true;
		}

		return false;
	});

	// Definitely not on a RPi4.
	if (!rpiMboxNode) return;
	if (rpiMboxNode->reg().size() != 1) return;

	globalMbox.initialize(rpiMboxNode->reg()[0].addr);

	debugLogger()
		<< "            Uploading VL805 firmware via Raspberry Pi4 firmware interface."
		<< frg::endlog;

	uint32_t addr = (dev->bus << 20) | (dev->slot << 15) | (dev->function << 12);

	uint32_t data[] = {
		0x00030058, // RPI_FIRMWARE_NOTIFY_XHCI_RESET
		sizeof(uint32_t), // request data size
		0, // response data size
		addr
	};

	globalMbox->sendPropertyList(frg::span<uint32_t>(data, 4));
}

#endif // THOR_HAS_DTB_SUPPORT

} } // namespace thor::pci
