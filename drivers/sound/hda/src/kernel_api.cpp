#include <iostream>
#include <unordered_map>
#include <pthread.h>

#include <uhda/kernel_api.h>

#include <protocols/hw/client.hpp>

#include <helix/timer.hpp>
#include <helix/memory.hpp>

#include "controller.hpp"

namespace {

template<typename T>
async::result<void> pciWrite(protocols::hw::Device &dev, uint8_t offset, T value) {
	co_await dev.storePciSpace(offset, sizeof(T), value);
}

template<typename T>
async::result<T> pciRead(protocols::hw::Device &dev, uint8_t offset) {
	co_return co_await dev.loadPciSpace(offset, sizeof(T));
}

async::result<void *> mapBar(protocols::hw::Device &dev, int bar) {
	auto handle = co_await dev.accessBar(bar);

	auto info = co_await dev.getPciInfo();

	void *barAddress;

	HEL_CHECK(helMapMemory(handle.getHandle(), kHelNullHandle, nullptr,
		0, info.barInfo[bar].length, kHelMapProtRead | kHelMapProtWrite, &barAddress));

	co_return barAddress;
}

async::result<void> unmapBar(protocols::hw::Device &dev, int bar, void *ptr) {
	auto info = co_await dev.getPciInfo();

	HEL_CHECK(helUnmapMemory(kHelNullHandle, ptr, info.barInfo[bar].length));
}

} // namespace

UhdaStatus uhda_kernel_pci_read(uintptr_t handle, uint8_t offset, uint8_t size, uint32_t *res) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	switch (size) {
	case 1:
		*res = async::run(pciRead<uint8_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	case 2:
		*res = async::run(pciRead<uint16_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	case 4:
		*res = async::run(pciRead<uint32_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	default:
		return UHDA_STATUS_UNSUPPORTED;
	}

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_kernel_pci_write(uintptr_t handle, uint8_t offset, uint8_t size, uint32_t value) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	switch (size) {
	case 1:
		async::run(pciWrite<uint8_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	case 2:
		async::run(pciWrite<uint16_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	case 4:
		async::run(pciWrite<uint32_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	default:
		return UHDA_STATUS_UNSUPPORTED;
	}

	return UHDA_STATUS_SUCCESS;
}

async::detached handleIrqs(helix::BorrowedDescriptor irq, UhdaIrqHandlerFn fn, void *arg);

UhdaStatus uhda_kernel_pci_allocate_irq(uintptr_t handle, UhdaIrqHint hint, UhdaIrqHandlerFn fn, void *arg, void **opaque_irq) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	helix::UniqueDescriptor irq;
	if (ctrl->msiAvailable && hint != UHDA_IRQ_HINT_INTX) {
		ctrl->useMsi = true;
		irq = async::run(ctrl->device.installMsi(0), helix::currentDispatcher);
	} else {
		irq = async::run(ctrl->device.accessIrq(), helix::currentDispatcher);
	}

	handleIrqs(irq, fn, arg);

	*opaque_irq = reinterpret_cast<void *>(irq.getHandle());
	irq.release();

	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_deallocate_irq(uintptr_t, void *opaque_irq) {
	helix::UniqueDescriptor irq{reinterpret_cast<HelHandle>(opaque_irq)};
}

void uhda_kernel_pci_enable_irq(uintptr_t handle, void *, bool enable) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	if (enable) {
		if (ctrl->useMsi) {
			async::run(ctrl->device.enableMsi(), helix::currentDispatcher);
		} else {
			async::run(ctrl->device.enableBusIrq(), helix::currentDispatcher);
		}
	}
}

UhdaStatus uhda_kernel_pci_map_bar(uintptr_t handle, uint32_t bar, void **virt) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	*virt = async::run(mapBar(ctrl->device, bar), helix::currentDispatcher);
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_unmap_bar(uintptr_t handle, uint32_t bar, void* virt) {
	auto ctrl = reinterpret_cast<Controller *>(handle);

	async::run(unmapBar(ctrl->device, bar, virt), helix::currentDispatcher);
}

void* uhda_kernel_malloc(size_t size) {
	return malloc(size);
}

void uhda_kernel_free(void *ptr, size_t) {
	free(ptr);
}

void uhda_kernel_delay(uint32_t microseconds) {
	async::run(helix::sleepFor(microseconds * 1000), helix::currentDispatcher);
}

void uhda_kernel_log(const char *str) {
	std::cout << "sound/hda: uHDA: " << str << std::endl;
}

UhdaStatus uhda_kernel_allocate_physical(uintptr_t handle, size_t size, uintptr_t *res) {
	auto controller = reinterpret_cast<Controller *>(handle);

	arch::dma_buffer buf{controller->pool, size};
	auto iova = async::run(controller->dmaSpace.iova_of(buf), helix::currentDispatcher);
	controller->physicalMappings.emplace(iova, std::move(buf));

	*res = iova;
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_deallocate_physical(uintptr_t handle, uintptr_t phys, size_t size) {
	auto controller = reinterpret_cast<Controller *>(handle);
	auto mapping = controller->physicalMappings.find(phys);
	assert(mapping != controller->physicalMappings.end());
	assert(mapping->second.size() == size);
	controller->physicalMappings.erase(mapping);
}

extern std::vector<arch::dma_buffer_view> globalPeriodChunks;

UhdaStatus uhda_kernel_allocate_scatter(uintptr_t handle, size_t count, size_t size, UhdaScatterChunk *res) {
	(void) size;
	auto controller = reinterpret_cast<Controller *>(handle);

	assert(count == globalPeriodChunks.size());

	for (size_t i = 0; i < count; i++) {
		auto &chunk = res[i];
		chunk.virt = globalPeriodChunks[i].data();
		chunk.phys = async::run(controller->dmaSpace.iova_of(globalPeriodChunks[i]), helix::currentDispatcher);
	}

	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_deallocate_scatter(UhdaScatterChunk *chunks, size_t count, size_t size) {
	(void) chunks;
	(void) count;
	(void) size;
}

UhdaStatus uhda_kernel_map(uintptr_t handle, uintptr_t phys, size_t, void **virt) {
	auto controller = reinterpret_cast<Controller *>(handle);
	auto mapping = controller->physicalMappings.find(phys);
	assert(mapping != controller->physicalMappings.end());

	*virt = mapping->second.data();
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_unmap(void *, size_t) {}

UhdaStatus uhda_kernel_create_spinlock(void **spinlock) {
	auto lock = new pthread_spinlock_t{};
	pthread_spin_init(lock, PTHREAD_PROCESS_PRIVATE);
	*spinlock = lock;
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_free_spinlock(void *spinlock) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_destroy(lock);
	delete lock;
}

UhdaIrqState uhda_kernel_lock_spinlock(void *spinlock) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_lock(lock);
	return 0;
}

void uhda_kernel_unlock_spinlock(void* spinlock, UhdaIrqState) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_unlock(lock);
}
