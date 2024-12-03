#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir/interface.hpp>
#include <frg/cmdline.hpp>
#include <stdint.h>
#include <uacpi/acpi.h>

#include "limine.h"

namespace eir {

namespace {

#define LIMINE_REQUEST(request, tag, rev)                                                          \
	[[gnu::used, gnu::section(".requests")]]                                                       \
	volatile struct limine_##request request = {                                                   \
	    .id = tag,                                                                                 \
	    .revision = rev,                                                                           \
	    .response = nullptr,                                                                       \
	}

[[gnu::used, gnu::section(".requestsStartMarker")]] volatile LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".requests")]] volatile LIMINE_BASE_REVISION(3);
LIMINE_REQUEST(memmap_request, LIMINE_MEMMAP_REQUEST, 0);
LIMINE_REQUEST(hhdm_request, LIMINE_HHDM_REQUEST, 0);
LIMINE_REQUEST(smp_request, LIMINE_SMP_REQUEST, 0);
LIMINE_REQUEST(framebuffer_request, LIMINE_FRAMEBUFFER_REQUEST, 1);
LIMINE_REQUEST(module_request, LIMINE_MODULE_REQUEST, 0);
LIMINE_REQUEST(kernel_file_request, LIMINE_KERNEL_FILE_REQUEST, 0);
LIMINE_REQUEST(kernel_address_request, LIMINE_KERNEL_ADDRESS_REQUEST, 0);
LIMINE_REQUEST(rsdp_request, LIMINE_RSDP_REQUEST, 0);
LIMINE_REQUEST(dtb_request, LIMINE_DTB_REQUEST, 0);
[[gnu::used, gnu::section(".requestsEndMarker")]] volatile LIMINE_REQUESTS_END_MARKER;

initgraph::Task setupMiscInfo{
    &globalInitEngine,
    "limine.setup-misc-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
#ifdef __riscv
	    info_ptr->hartId = smp_request.response->bsp_hartid;
#endif

	    if (dtb_request.response) {
		    DeviceTree dt{dtb_request.response->dtb_ptr};
		    info_ptr->dtbPtr = virtToPhys(dtb_request.response->dtb_ptr);
		    info_ptr->dtbSize = dt.size();
	    }

	    if (rsdp_request.response) {
		    info_ptr->acpiRsdp = virtToPhys(rsdp_request.response->address);
	    }
    }
};

initgraph::Task setupFramebufferInfo{
    &globalInitEngine,
    "limine.setup-framebuffer-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0
	        && framebuffer_request.response->framebuffers) {
		    auto *limine_fb = framebuffer_request.response->framebuffers[0];
		    fb = &info_ptr->frameBuffer;
		    fb->fbAddress = virtToPhys(limine_fb->address);
		    fb->fbPitch = limine_fb->pitch;
		    fb->fbWidth = limine_fb->width;
		    fb->fbHeight = limine_fb->height;
		    fb->fbBpp = limine_fb->bpp;
		    fb->fbType = limine_fb->memory_model; // TODO: Maybe inaccurate
	    } else {
		    infoLogger() << "eir: Got no framebuffer!" << frg::endlog;
	    }
    }
};

initgraph::Task setupMemoryRegions{
    &globalInitEngine,
    "limine.setup-memory-regions",
    initgraph::Requires{getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    assert(memmap_request.response);

	    eir::infoLogger() << "Memory map:" << frg::endlog;
	    for (size_t entry = 0; entry < memmap_request.response->entry_count; entry++) {
		    auto *map = memmap_request.response->entries[entry];
		    eir::infoLogger() << "    Type " << map->type << " mapping."
		                      << " Base: 0x" << frg::hex_fmt{map->base} << ", length: 0x"
		                      << frg::hex_fmt{map->length} << frg::endlog;
		    if (map->type == LIMINE_MEMMAP_USABLE
		        || map->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
			    createInitialRegions(
			        {map->base, map->length}, {reservedRegions.data(), nReservedRegions}
			    );
	    }
    }
};

initgraph::Task setupInitrdInfo{
    &globalInitEngine,
    "limine.setup-initrd-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    auto initrd_module = bootAlloc<EirModule>(1);
	    initrd_module->physicalBase = virtToPhys(initrd);
	    initrd_module->length = module_request.response->modules[0]->size;
	    const char *initrd_mod_name = "initrd.cpio";
	    size_t name_length = strlen(initrd_mod_name);
	    char *name_ptr = bootAlloc<char>(name_length);
	    memcpy(name_ptr, initrd_mod_name, name_length);
	    initrd_module->namePtr = mapBootstrapData(name_ptr);
	    initrd_module->nameLength = name_length;

	    info_ptr->moduleInfo = mapBootstrapData(initrd_module);
    }
};

} // namespace

extern "C" void eirLimineMain(void) {
	eir::infoLogger() << "Booting Eir from Limine" << frg::endlog;
	eirRunConstructors();

	if (dtb_request.response) {
		eirDtbPtr = dtb_request.response->dtb_ptr;
		eir::infoLogger() << "DTB accessible at " << eirDtbPtr << frg::endlog;
	} else {
		eir::infoLogger() << "Limine did not pass a DTB" << frg::endlog;
	}

	assert(kernel_file_request.response);
	cmdline = frg::string_view{kernel_file_request.response->kernel_file->cmdline};
	eir::infoLogger() << "Command line: " << cmdline << frg::endlog;

	frg::array args = {
	    frg::option{"bochs", frg::store_true(log_e9)},
	};
	frg::parse_arguments(cmdline, args);

	assert(module_request.response);
	assert(module_request.response->module_count > 0);
	auto initrd_file = module_request.response->modules[0];

	initrd = initrd_file->address;
	physOffset = hhdm_request.response->offset;
	kernel_physical = kernel_address_request.response->physical_base;

	eirMain();
}

} // namespace eir
