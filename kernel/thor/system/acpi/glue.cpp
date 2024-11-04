#include <limits.h>

#include <async/mutex.hpp>
#include <async/queue.hpp>
#include <async/recurring-event.hpp>
#include <frg/allocation.hpp>
#include <frg/manual_box.hpp>
#include <frg/spinlock.hpp>
#include <frg/string.hpp>
#include <stdlib.h>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/pci/pci.hpp>

#include <uacpi/kernel_api.h>

using namespace thor;

void uacpi_kernel_log(enum uacpi_log_level lvl, const char *msg) {
	const char *lvlStr;

	switch (lvl) {
	case UACPI_LOG_TRACE:
		lvlStr = "trace";
		break;
	case UACPI_LOG_INFO:
		lvlStr = "info";
		break;
	case UACPI_LOG_WARN:
		lvlStr = "warn";
		break;
	case UACPI_LOG_ERROR:
		lvlStr = "error";
		break;
	default:
		lvlStr = "<invalid>";
	}

	auto msgView = frg::string_view(msg);
	if (msgView.ends_with("\n"))
		msgView = msgView.sub_string(0, msgView.size() - 1);

	infoLogger() << "uacpi-" << lvlStr << ": " << msgView << frg::endlog;
}

void *uacpi_kernel_alloc(uacpi_size size) { return kernelAlloc->allocate(size); }

void *uacpi_kernel_calloc(uacpi_size count, uacpi_size size) {
	auto bytes = count * size;

	auto *ptr = uacpi_kernel_alloc(bytes);
	if (ptr == nullptr)
		return ptr;

	memset(ptr, 0, bytes);
	return ptr;
}

void uacpi_kernel_free(void *ptr) { kernelAlloc->free(ptr); }

// TODO: We do not want to keep things mapped forever.
void *uacpi_kernel_map(uacpi_phys_addr physical, uacpi_size length) {
	auto pow2ceil = [](unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto paddr = physical & ~(kPageSize - 1);
	auto vsize = length + (physical & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	auto ptr = KernelVirtualMemory::global().allocate(msize);
	for (size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(
		    (VirtualAddr)ptr + pg, paddr + pg, page_access::write, CachingMode::null
		);
	return reinterpret_cast<char *>(ptr) + (physical & (kPageSize - 1));
}

void uacpi_kernel_unmap(void *ptr, uacpi_size length) {
	auto pow2ceil = [](unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto vaddr = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
	auto vsize = length + (reinterpret_cast<uintptr_t>(ptr) & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	for (size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().unmapSingle4k(vaddr + pg);
	// TODO: free the virtual memory range.
	(void)msize;
}

uacpi_status
uacpi_kernel_raw_memory_read(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 *out) {
	auto *ptr = uacpi_kernel_map(address, byte_width);

	switch (byte_width) {
	case 1:
		*out = *(volatile uint8_t *)ptr;
		break;
	case 2:
		*out = *(volatile uint16_t *)ptr;
		break;
	case 4:
		*out = *(volatile uint32_t *)ptr;
		break;
	case 8:
		*out = *(volatile uint64_t *)ptr;
		break;
	default:
		uacpi_kernel_unmap(ptr, byte_width);
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	uacpi_kernel_unmap(ptr, byte_width);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_raw_memory_write(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 in) {
	auto *ptr = uacpi_kernel_map(address, byte_width);

	switch (byte_width) {
	case 1:
		*(volatile uint8_t *)ptr = in;
		break;
	case 2:
		*(volatile uint16_t *)ptr = in;
		break;
	case 4:
		*(volatile uint32_t *)ptr = in;
		break;
	case 8:
		*(volatile uint64_t *)ptr = in;
		break;
	default:
		uacpi_kernel_unmap(ptr, byte_width);
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	uacpi_kernel_unmap(ptr, byte_width);
	return UACPI_STATUS_OK;
}

#ifdef THOR_ARCH_SUPPORTS_PIO
uacpi_status
uacpi_kernel_raw_io_write(uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 in_value) {
	uint16_t p = address;

	switch (byte_width) {
	case 1: {
		uint8_t v = in_value;
		asm volatile("outb %0, %1" : : "a"(v), "d"(p));
		break;
	}
	case 2: {
		uint16_t v = in_value;
		asm volatile("outw %0, %1" : : "a"(v), "d"(p));
		break;
	}
	case 4: {
		uint32_t v = in_value;
		asm volatile("outl %0, %1" : : "a"(v), "d"(p));
		break;
	}
	default:
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_raw_io_read(uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value) {
	uint16_t p = address;

	switch (byte_width) {
	case 1: {
		uint8_t v;
		asm volatile("inb %1, %0" : "=a"(v) : "d"(p));
		*out_value = v;
		break;
	}
	case 2: {
		uint16_t v;
		asm volatile("inw %1, %0" : "=a"(v) : "d"(p));
		*out_value = v;
		break;
	}
	case 4: {
		uint32_t v;
		asm volatile("inl %1, %0" : "=a"(v) : "d"(p));
		*out_value = v;
		break;
	}
	default:
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}
#else
uacpi_status uacpi_kernel_raw_io_read(uacpi_io_addr, uacpi_u8, uacpi_u64 *) {
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_raw_io_write(uacpi_io_addr, uacpi_u8, uacpi_u64) {
	return UACPI_STATUS_UNIMPLEMENTED;
}

#endif

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size, uacpi_handle *out_handle) {
	*out_handle = reinterpret_cast<uacpi_handle>(base);
	return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle) {}

uacpi_status uacpi_kernel_io_read(
    uacpi_handle handle, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 *value
) {
	auto addr = reinterpret_cast<uacpi_io_addr>(handle);
	return uacpi_kernel_raw_io_read(addr + offset, byte_width, value);
}

uacpi_status uacpi_kernel_io_write(
    uacpi_handle handle, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 value
) {
	auto addr = reinterpret_cast<uacpi_io_addr>(handle);
	return uacpi_kernel_raw_io_write(addr + offset, byte_width, value);
}

uacpi_status uacpi_kernel_pci_read(
    uacpi_pci_address *address, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 *value
) {
	switch (byte_width) {
	case 1: {
		*value = pci::readConfigByte(
		    address->segment, address->bus, address->device, address->function, offset
		);
		break;
	}
	case 2: {
		*value = pci::readConfigHalf(
		    address->segment, address->bus, address->device, address->function, offset
		);
		break;
	}
	case 4: {
		*value = pci::readConfigWord(
		    address->segment, address->bus, address->device, address->function, offset
		);
		break;
	}
	default:
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write(
    uacpi_pci_address *address, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 value
) {
	switch (byte_width) {
	case 1: {
		pci::writeConfigByte(
		    address->segment, address->bus, address->device, address->function, offset, value
		);
		break;
	}
	case 2: {
		pci::writeConfigHalf(
		    address->segment, address->bus, address->device, address->function, offset, value
		);
		break;
	}
	case 4: {
		pci::writeConfigWord(
		    address->segment, address->bus, address->device, address->function, offset, value
		);
		break;
	}
	default:
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}

uacpi_u64 uacpi_kernel_get_ticks(void) { return systemClockSource()->currentNanos() / 100; }

void uacpi_kernel_stall(uacpi_u8 usec) {
	auto now = systemClockSource()->currentNanos();
	auto deadline = now + usec * 1000;

	while (systemClockSource()->currentNanos() < deadline)
		;
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(msec * 1000 * 1000));
}

struct SciDevice final : IrqSink {
	uacpi_interrupt_handler handler;
	uacpi_handle ctx;

	SciDevice() : IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "acpi-sci"}} {}

	IrqStatus raise() override {
		return handler(ctx) & UACPI_INTERRUPT_HANDLED ? IrqStatus::acked : IrqStatus::nacked;
	}
};

frg::manual_box<SciDevice> sciDevice;

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx, uacpi_handle *out_irq_handle
) {
	auto sciOverride = resolveIsaIrq(irq);
	configureIrq(sciOverride);

	sciDevice.initialize();
	sciDevice->handler = handler;
	sciDevice->ctx = ctx;

#ifdef __x86_64__
	IrqPin::attachSink(getGlobalSystemIrq(sciOverride.gsi), sciDevice.get());
#endif

	*out_irq_handle = &sciDevice;
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle) {
	return UACPI_STATUS_UNIMPLEMENTED;
}

struct AcpiWork {
	uacpi_work_handler handler_;
	uacpi_handle ctx_;
};
static async::queue<AcpiWork, KernelAlloc> acpiGpeWorkQueue = {*kernelAlloc};
static async::queue<AcpiWork, KernelAlloc> acpiNotifyWorkQueue = {*kernelAlloc};
static async::recurring_event acpiWorkEvent;
static std::atomic<uint64_t> acpiWorkCounter;

static void workExec(AcpiWork &work) {
	work.handler_(work.ctx_);
	acpiWorkCounter.fetch_sub(1, std::memory_order_acq_rel);
	acpiWorkEvent.raise();
}

void thor::acpi::initGlue() {
	KernelFiber::run(
	    [] {
		    while (true) {
			    auto work = KernelFiber::asyncBlockCurrent(acpiGpeWorkQueue.async_get());
			    workExec(*work);
		    }
	    },
	    &getCpuData(0)->scheduler
	);

	KernelFiber::run([] {
		while (true) {
			auto work = KernelFiber::asyncBlockCurrent(acpiNotifyWorkQueue.async_get());
			workExec(*work);
		}
	});
}

uacpi_status
uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx) {
	acpiWorkCounter.fetch_add(1, std::memory_order_acq_rel);

	switch (type) {
	case UACPI_WORK_GPE_EXECUTION:
		acpiGpeWorkQueue.put({handler, ctx});
		break;
	case UACPI_WORK_NOTIFICATION:
		acpiNotifyWorkQueue.put({handler, ctx});
		break;
	default:
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
	KernelFiber::asyncBlockCurrent([]() -> coroutine<void> {
		while (acpiWorkCounter.load(std::memory_order_acquire))
			co_await acpiWorkEvent.async_wait();
	}());
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
	switch (req->type) {
	case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
		infoLogger() << "thor: ignoring AML breakpoint" << frg::endlog;
		break;
	case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
		infoLogger() << "thor: fatal firmware error:"
		             << " type: " << (int)req->fatal.type << " code: " << req->fatal.code
		             << " arg: " << req->fatal.arg << frg::endlog;
		break;
	}

	return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_mutex(void) { return frg::construct<async::mutex>(*kernelAlloc); }
void uacpi_kernel_free_mutex(uacpi_handle opaque) {
	frg::destruct(*kernelAlloc, reinterpret_cast<async::mutex *>(opaque));
}

uacpi_bool uacpi_kernel_acquire_mutex(uacpi_handle opaque, uacpi_u16 timeout) {
	auto *mutex = reinterpret_cast<async::mutex *>(opaque);

	if (timeout == 0xFFFF) {
		KernelFiber::asyncBlockCurrent([mutex]() -> coroutine<void> {
			co_await mutex->async_lock();
		}());
		return true;
	}

	uacpi_u16 sleepTime;
	do {
		if (mutex->try_lock())
			return true;

		sleepTime = frg::min<uacpi_u16>(timeout, 10);
		timeout -= sleepTime;

		if (sleepTime)
			uacpi_kernel_sleep(sleepTime);
	} while (timeout);

	return false;
}

void uacpi_kernel_release_mutex(uacpi_handle opaque) {
	auto *mutex = reinterpret_cast<async::mutex *>(opaque);
	mutex->unlock();
}

struct AcpiEvent {
	std::atomic<uint64_t> counter;

	bool tryDecrement() {
		for (;;) {
			auto value = counter.load(std::memory_order::acquire);
			if (value == 0)
				return false;

			if (counter.compare_exchange_strong(
			        value, value - 1, std::memory_order::acq_rel, std::memory_order::acquire
			    ))
				return true;
		}
	}
};

uacpi_handle uacpi_kernel_create_event(void) { return frg::construct<AcpiEvent>(*kernelAlloc); }
void uacpi_kernel_free_event(uacpi_handle opaque) {
	frg::destruct(*kernelAlloc, reinterpret_cast<AcpiEvent *>(opaque));
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle opaque, uacpi_u16 timeout) {
	auto *event = reinterpret_cast<AcpiEvent *>(opaque);

	uacpi_u16 sleepTime;
	do {
		if (event->tryDecrement())
			return true;

		sleepTime = frg::min<uacpi_u16>(timeout, 10);

		if (timeout != 0xFFFF)
			timeout -= sleepTime;

		if (sleepTime)
			uacpi_kernel_sleep(sleepTime);
	} while (timeout);

	return false;
}

void uacpi_kernel_signal_event(uacpi_handle opaque) {
	auto *event = reinterpret_cast<AcpiEvent *>(opaque);
	event->counter.fetch_add(1, std::memory_order_acq_rel);
}
void uacpi_kernel_reset_event(uacpi_handle opaque) {
	auto *event = reinterpret_cast<AcpiEvent *>(opaque);
	event->counter.store(0, std::memory_order_release);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return thisFiber(); }

uacpi_handle uacpi_kernel_create_spinlock(void) {
	return frg::construct<IrqSpinlock>(*kernelAlloc);
}
void uacpi_kernel_free_spinlock(uacpi_handle opaque) {
	frg::destruct(*kernelAlloc, reinterpret_cast<IrqSpinlock *>(opaque));
}

uacpi_cpu_flags uacpi_kernel_spinlock_lock(uacpi_handle opaque) {
	auto *lock = reinterpret_cast<IrqSpinlock *>(opaque);

	lock->lock();

	// IrqSpinlock already manages turning off interrupts, so no
	// need to track that here.
	return 0;
}

void uacpi_kernel_spinlock_unlock(uacpi_handle opaque, uacpi_cpu_flags) {
	auto *mutex = reinterpret_cast<IrqSpinlock *>(opaque);
	mutex->unlock();
}
