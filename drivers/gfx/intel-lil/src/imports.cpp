#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <async/result.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <protocols/hw/client.hpp>

#include <lil/imports.h>
#include <lil/vbt.h>
#include <lil/vbt-types.h>

#include "debug.hpp"

namespace {

async::result<std::pair<void *, uintptr_t>> readBar(protocols::hw::Device *dev, int bar, uintptr_t len) {
	auto bar_handle = co_await dev->accessBar(bar);

	if(!len) {
		auto info = co_await dev->getPciInfo();
		len = info.barInfo[bar].length;
	}

	void *bar_addr;

	HEL_CHECK(helMapMemory(bar_handle.getHandle(), kHelNullHandle, nullptr,
		0, len, kHelMapProtRead | kHelMapProtWrite, &bar_addr));

	co_return {bar_addr, len};
}

template<typename T>
	requires (std::is_integral<T>::value)
async::result<void> pciWrite(protocols::hw::Device *dev, uint16_t offset, T val) {
	co_await dev->storePciSpace(offset, sizeof(T), val);
}

template<typename T>
	requires (std::is_integral<T>::value)
async::result<T> pciRead(protocols::hw::Device *dev, uint16_t offset) {
	co_return T(co_await dev->loadPciSpace(offset, sizeof(T)));
}

}

void lil_pci_writeb(void *device, uint16_t offset, uint8_t val) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	async::run(pciWrite<uint8_t>(dev, offset, val), helix::currentDispatcher);
}

void lil_pci_writew(void *device, uint16_t offset, uint16_t val) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	async::run(pciWrite<uint16_t>(dev, offset, val), helix::currentDispatcher);
}

void lil_pci_writed(void *device, uint16_t offset, uint32_t val) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	async::run(pciWrite<uint32_t>(dev, offset, val), helix::currentDispatcher);
}

uint8_t lil_pci_readb(void *device, uint16_t offset) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	return async::run(pciRead<uint8_t>(dev, offset), helix::currentDispatcher);
}
uint16_t lil_pci_readw(void *device, uint16_t offset) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	return async::run(pciRead<uint16_t>(dev, offset), helix::currentDispatcher);
}
uint32_t lil_pci_readd(void *device, uint16_t offset) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	return async::run(pciRead<uint32_t>(dev, offset), helix::currentDispatcher);
}

void lil_sleep(uint64_t ms) {
	async::run(helix::sleepFor(ms * 1000 * 1000), helix::currentDispatcher);
}

void lil_usleep(uint64_t us) {
	async::run(helix::sleepFor(us * 1000), helix::currentDispatcher);
}

void lil_get_bar(void *device, int bar, uintptr_t *obase, uintptr_t *len) {
	auto dev = static_cast<protocols::hw::Device *>(device);

	auto [bar_addr, bar_len] = async::run(readBar(dev, bar, *len), helix::currentDispatcher);

	*obase = uintptr_t(bar_addr);
	*len = bar_len;
}

void *lil_malloc(size_t s) {
	return malloc(s);
}

void lil_free(void *p) {
	free(p);
}

void lil_log(enum LilLogType type, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	switch(type) {
		case ERROR:
			printf("\e[31mlil: ");
			break;
		case WARNING:
			printf("\e[33mlil: ");
			break;
		case INFO:
			if(logLilVerbose) {
				printf("lil: ");
			} else {
				va_end(args);
				return;
			}
			break;
		case DEBUG:
		case VERBOSE:
			if(logLilVerbose) {
				printf("\e[37mlil: ");
			} else {
				va_end(args);
				return;
			}
			break;
	}

	vprintf(fmt, args);
	printf("\e[39m");

	va_end(args);
}

void lil_panic(const char* msg) {
	printf("\e[31m%s\n\e[39m", msg);

	exit(1);
}

const struct vbt_header *lil_vbt_locate(LilGpu *gpu) {
	auto dev = reinterpret_cast<protocols::hw::Device *>(gpu->dev);

	auto [desc, vbt_size] = async::run(dev->getVbt(), helix::currentDispatcher);

	void *vbt;
	HEL_CHECK(helMapMemory(desc.getHandle(), kHelNullHandle,
		nullptr, 0, (vbt_size + 0xFFF) & ~0xFFF,
		kHelMapProtRead | kHelMapProtWrite, &vbt));

	const struct vbt_header *vbt_header = vbt_get_header(vbt, vbt_size);
	if(logLilVerbose) {
		printf("lil: VBT addr 0x%lx\n", (uintptr_t) vbt_header);
		printf("lil: VBT @ %lx (signature %.20s)\n", (uintptr_t) vbt_header, vbt_header->signature);
	}

	if(!vbt_header) {
		lil_panic("no VBT found");
	}

	return vbt_header;
}
