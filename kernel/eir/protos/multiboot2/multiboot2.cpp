#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/spec.hpp>
#include <eir/interface.hpp>
#include <stdint.h>
#include <uacpi/acpi.h>

namespace eir {

namespace {

eir::Mb2Info *mbInfo = nullptr;

uintptr_t mmapStart = 0;
uintptr_t mmapEnd = 0;

eir::Mb2TagFramebuffer *framebuffer = nullptr;
eir::Mb2Tag *acpiTag = nullptr;

initgraph::Task setupAcpiInfo{
    &globalInitEngine,
    "mb2.setup-acpi-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    if (acpiTag) {
		    auto *rsdpPtr = bootAlloc<uint8_t>(acpiTag->size - sizeof(Mb2TagRSDP));
		    memcpy(rsdpPtr, acpiTag->data, acpiTag->size - sizeof(Mb2TagRSDP));
		    info_ptr->acpiRsdp = reinterpret_cast<uint64_t>(rsdpPtr);
	    }
    }
};

initgraph::Task setupFramebufferInfo{
    &globalInitEngine,
    "mb2.setup-framebuffer-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    if (framebuffer) {
		    fb = &info_ptr->frameBuffer;
		    fb->fbAddress = framebuffer->address;
		    fb->fbPitch = framebuffer->pitch;
		    fb->fbWidth = framebuffer->width;
		    fb->fbHeight = framebuffer->height;
		    fb->fbBpp = framebuffer->bpp;
		    fb->fbType = framebuffer->type;
	    }
    }
};

initgraph::Task setupMemoryRegions{
    &globalInitEngine,
    "mb2.setup-memory-regions",
    initgraph::Requires{getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    assert(mmapStart);
	    assert(mmapEnd > mmapStart);
	    assert(cmdline.data()); // Make sure it at least exists

	    eir::infoLogger() << "Command line: " << cmdline << frg::endlog;

	    eir::infoLogger() << "Memory map:" << frg::endlog;
	    for (Mb2MmapEntry *map = (Mb2MmapEntry *)mmapStart; map < (Mb2MmapEntry *)mmapEnd; map++) {
		    eir::infoLogger() << "    Type " << map->type << " mapping."
		                      << " Base: 0x" << frg::hex_fmt{map->base} << ", length: 0x"
		                      << frg::hex_fmt{map->length} << frg::endlog;
		    if (map->type == 1)
			    createInitialRegions(
			        {map->base, map->length}, {reservedRegions.data(), nReservedRegions}
			    );
	    }
    }
};

initgraph::Task setupInitrdInfo{
    &globalInitEngine,
    "mb2.setup-initrd-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    auto initrd_module = bootAlloc<EirModule>(1);
	    size_t add_size = 0;

	    for (size_t i = 8 /* Skip size and reserved fields*/; i < mbInfo->size; i += add_size) {
		    Mb2Tag *tag = (Mb2Tag *)((uint8_t *)mbInfo + i);

		    if (tag->type == kMb2TagEnd)
			    break;

		    add_size = tag->size;
		    if ((add_size % 8) != 0)
			    add_size += (8 - add_size % 8); // Align 8byte

		    switch (tag->type) {
			    case kMb2TagModule: {
				    auto *module = reinterpret_cast<Mb2TagModule *>(tag);

				    initrd_module->physicalBase = (EirPtr)module->start;
				    initrd_module->length = (EirPtr)module->end - (EirPtr)module->start;

				    size_t name_length = strlen(module->string);
				    char *name_ptr = bootAlloc<char>(name_length);
				    memcpy(name_ptr, module->string, name_length);
				    initrd_module->namePtr = mapBootstrapData(name_ptr);
				    initrd_module->nameLength = name_length;
				    break;
			    }
		    }
	    }

	    info_ptr->moduleInfo = mapBootstrapData(initrd_module);
    }
};

} // namespace

extern "C" void eirMultiboot2Main(uint32_t info, uint32_t magic) {
	if (magic != MB2_MAGIC)
		eir::panicLogger() << "eir: Invalid multiboot2 signature, halting..." << frg::endlog;

	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {0, eirEnd};

	mbInfo = reinterpret_cast<Mb2Info *>(info);
	size_t add_size = 0;
	size_t n_modules = 0;

	for (size_t i = 8 /* Skip size and reserved fields*/; i < mbInfo->size; i += add_size) {
		Mb2Tag *tag = (Mb2Tag *)((uint8_t *)info + i);

		if (tag->type == kMb2TagEnd)
			break;

		add_size = tag->size;
		if ((add_size % 8) != 0)
			add_size += (8 - add_size % 8); // Align 8byte

		switch (tag->type) {
			case kMb2TagFramebuffer: {
				auto *framebuffer_tag = reinterpret_cast<Mb2TagFramebuffer *>(tag);
				if (framebuffer_tag->address + framebuffer_tag->width * framebuffer_tag->pitch
				    >= UINTPTR_MAX) {
					eir::infoLogger()
					    << "eir: Framebuffer outside of addressable memory!" << frg::endlog;
					framebuffer = framebuffer_tag;
				} else if (framebuffer_tag->bpp != 32) {
					eir::infoLogger() << "eir: Framebuffer does not use 32 bpp!" << frg::endlog;
				} else {
					setFbInfo(
					    reinterpret_cast<void *>(framebuffer_tag->address),
					    framebuffer_tag->width,
					    framebuffer_tag->height,
					    framebuffer_tag->pitch
					);

					framebuffer = framebuffer_tag;
				}
				break;
			}

			case kMb2TagModule: {
				auto *module = reinterpret_cast<Mb2TagModule *>(tag);

				if (n_modules)
					eir::panicLogger() << "eir: only one module is supported!" << frg::endlog;

				initrd = reinterpret_cast<void *>(module->start);
				reservedRegions[nReservedRegions++] = {module->start, module->end - module->start};

				n_modules++;
				break;
			}

			case kMb2TagMmap: {
				auto *mmap = reinterpret_cast<Mb2TagMmap *>(tag);

				mmapStart = (uintptr_t)mmap->entries;
				mmapEnd = (uintptr_t)mmap + mmap->length;

				break;
			}

			case kMb2TagCmdline: {
				auto *cmdline_tag = reinterpret_cast<Mb2TagCmdline *>(tag);

				cmdline = {cmdline_tag->string};

				break;
			}

			case kMb2TagAcpiOld:
			case kMb2TagAcpiNew: {
				acpiTag = tag;
				break;
			}
		}
	}

	eirMain();
}

} // namespace eir
