#ifdef __x86_64__
#include <arch/io_space.hpp>
#endif
#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/array.hpp>
#include <frg/cmdline.hpp>
#include <stdbool.h>

#include "efi.hpp"
#include "helpers.hpp"

static_assert(sizeof(char16_t) == sizeof(wchar_t), "Strings are not UTF-16-ish, are you missing -fshort-wchar?");

namespace eir {

const efi_system_table *st = nullptr;
const efi_boot_services *bs = nullptr;
efi_handle handle = nullptr;

namespace {

efi_graphics_output_protocol *gop = nullptr;
efi_loaded_image_protocol *loadedImage = nullptr;

frg::string_view initrd_path = "managarm\\initrd.cpio";
efi_file_info *initrdInfo = nullptr;

physaddr_t rsdp = 0;

size_t memMapSize = 0;
size_t mapKey = 0;
size_t descriptorSize = 0;
uint32_t descriptorVersion = 0;
void *memMap = nullptr;

// by reaching this we've performed all tasks that depend on EFI Boot Services
initgraph::Stage *getBootservicesDoneStage() {
	static initgraph::Stage s{&globalInitEngine, "uefi.bootservices-done"};
	return &s;
}

void uefiBootServicesLogHandler(const char c) {
	if(bs) {
		if(c == '\n') {
			frg::array<char16_t, 3> newline = {u"\r\n"};
			st->con_out->output_string(st->con_out, newline.data());
			return;
		}

		frg::array<char16_t, 2> converted = {static_cast<char16_t>(c), 0};
		st->con_out->output_string(st->con_out, converted.data());
	}
}

initgraph::Task findAcpi{&globalInitEngine,
	"uefi.find-acpi",
	initgraph::Entails{getBootservicesDoneStage()},
	[] {
		// acquire ACPI table info
		efi_guid acpi_guid = ACPI_20_TABLE_GUID;
		const efi_configuration_table *t = st->configuration_table;
		for(size_t i = 0; i < st->number_of_table_entries && t; i++, t++)
			if(!memcmp(&acpi_guid, &t->vendor_guid, sizeof(acpi_guid))) {
				rsdp = reinterpret_cast<physaddr_t>(t->vendor_table);
				infoLogger() << "eir: Got RSDP" << frg::endlog;
			}
	}
};

initgraph::Task findDtb{&globalInitEngine,
	"uefi.find-dtb",
	initgraph::Entails{getBootservicesDoneStage()},
	[] {
		// acquire ACPI table info
		efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
		const efi_configuration_table *t = st->configuration_table;
		for(size_t i = 0; i < st->number_of_table_entries && t; i++, t++)
			if(!memcmp(&dtb_guid, &t->vendor_guid, sizeof(dtb_guid))) {
				eirDtbPtr = physToVirt<void>(reinterpret_cast<physaddr_t>(t->vendor_table));
				infoLogger() << "eir: Got DTB" << frg::endlog;
			}
	}
};

#if defined(__riscv)
size_t boot_hart = 0;

initgraph::Task findRiscVBootHart{&globalInitEngine,
	"uefi.find-riscv-boot-hart",
	initgraph::Entails{getBootservicesDoneStage()},
	[] {
		riscv_efi_boot_protocol *boot_table = nullptr;
		efi_guid riscv_boot_guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
		auto status = bs->locate_protocol(&riscv_boot_guid, nullptr, (void **) &boot_table);
		assert(status == EFI_SUCCESS);
		assert(boot_table);

		status = boot_table->get_boot_hartid(boot_table, &boot_hart);
		assert(status == EFI_SUCCESS);

		eir::infoLogger() << "eir: boot HART ID " << boot_hart << frg::endlog;
	}
};

initgraph::Task setupBootHartId{&globalInitEngine,
	"uefi.setup-riscv-boot-hard-info",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		info_ptr->hartId = boot_hart;
	}
};
#endif

initgraph::Task readInitrd{&globalInitEngine,
	"uefi.read-initrd",
	initgraph::Entails{getBootservicesDoneStage()},
	[] {
		efi_file_protocol *initrdFile = nullptr;
		EFI_CHECK(fsOpen(&initrdFile, asciiToUcs2(initrd_path)));
		initrdInfo = fsGetInfo(initrdFile);

		// Read initrd.
		efi_physical_addr initrd_addr = 0;
		EFI_CHECK(bs->allocate_pages(AllocateAnyPages, EfiLoaderData, (initrdInfo->file_size >> 12) + 1, &initrd_addr));
		EFI_CHECK(fsRead(initrdFile, initrdInfo->file_size, 0, initrd_addr));

		initrd = reinterpret_cast<void *>(initrd_addr);
	}
};

initgraph::Task setupGop{&globalInitEngine,
	"uefi.setup-gop",
	initgraph::Entails{getBootservicesDoneStage()},
	[] {
		// Get the frame buffer.
		efi_guid gop_protocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
		efi_status status = bs->locate_protocol(&gop_protocol, nullptr, reinterpret_cast<void **>(&gop));

		if(gop->mode->info->version != 0)
			eir::panicLogger() << "error: unsupported EFI_GRAPHICS_OUTPUT_MODE_INFORMATION version!" << frg::endlog;

		if(status == EFI_SUCCESS) {
			eir::infoLogger() << "eir: framebuffer "
				<< gop->mode->info->horizontal_resolution << "x"
				<< gop->mode->info->vertical_resolution << " address=0x"
				<< frg::hex_fmt{gop->mode->framebuffer_base}
				<< frg::endlog;
		} else {
			// the spec claims that the `void **interface` argument will be a nullptr on
			// spec-listed error returns, but only lists two error codes; there are more
			// error codes in the wild, so best to not rely on them to return a nullptr
			gop = nullptr;
		}
	}
};

initgraph::Task exitBootServices{&globalInitEngine,
	"uefi.exit-boot-services",
	initgraph::Requires{getBootservicesDoneStage()},
	initgraph::Entails{getReservedRegionsKnownStage()},
	[] {
		memMapSize = sizeof(efi_memory_descriptor);
		efi_memory_descriptor dummy;

		// First get the size of the memory map buffer to allocate.
		efi_status status = bs->get_memory_map(&memMapSize, &dummy, &mapKey, &descriptorSize, &descriptorVersion);
		assert(status == EFI_BUFFER_TOO_SMALL);

		// the number of descriptors we overallocate the buffer by; get doubled every iteration
		size_t overallocation = 8;

		while(status != EFI_SUCCESS) {
			// needing more than that would be quite unreasonable
			assert(overallocation <= 0x800);

			// over-allocate a bit to accomodate the allocation we have to make here
			// we only get one shot(tm) to allocate an appropriately-sized buffer, as the spec does not
			// allow for calling any boot services other than GetMemoryMap and ExitBootServices after
			// a call to ExitBootServices fails
			memMapSize += overallocation * descriptorSize;
			EFI_CHECK(bs->allocate_pool(EfiLoaderData, memMapSize, &memMap));
			overallocation *= 2;

			// Now, get the actual memory map.
			EFI_CHECK(bs->get_memory_map(&memMapSize,
				reinterpret_cast<efi_memory_descriptor*>(memMap),
				&mapKey, &descriptorSize, &descriptorVersion));

			// Exit boot services.
			status = bs->exit_boot_services(handle, mapKey);
		}

		bs = nullptr;

#if defined(__x86_64__)
		asm volatile ("cli");
#elif defined(__riscv)
		asm volatile("csrci sstatus, 0x2" ::: "memory");
#else
#error "Unsupported architecture!"
#endif
	}
};

initgraph::Task setupMemoryMap{&globalInitEngine,
	"uefi.setup-memory-map",
	initgraph::Requires{&exitBootServices},
	initgraph::Entails{getReservedRegionsKnownStage()},
	[] {
		reservedRegions[nReservedRegions++] = {reinterpret_cast<uintptr_t>(loadedImage->image_base), loadedImage->image_size};
		reservedRegions[nReservedRegions++] = {reinterpret_cast<uintptr_t>(initrd), initrdInfo->file_size};

		auto entries = memMapSize / descriptorSize;

		auto endAddr = [](const efi_memory_descriptor *e) -> efi_physical_addr {
			return e->physical_start + (e->number_of_pages * eir::pageSize);
		};

		auto nextEntry = [&](efi_physical_addr addr) {
			const efi_memory_descriptor *lowest = nullptr;

			for(size_t i = 0; i < entries; i++) {
				auto e = reinterpret_cast<const efi_memory_descriptor *>(uintptr_t(memMap) + (i * descriptorSize));
				if(e->physical_start >= addr) {
					if(!lowest || e->physical_start < lowest->physical_start) {
						lowest = e;
					}
				}
			}

			return lowest;
		};

		auto isContiguous = [](const efi_memory_descriptor *a, const efi_memory_descriptor *b) {
			if(a->physical_start + (a->number_of_pages * eir::pageSize) == b->physical_start)
				return true;
			return false;
		};

		auto isUsable = [](const efi_memory_descriptor *e) {
			switch(e->type) {
				case EfiConventionalMemory:
				case EfiBootServicesCode:
				case EfiBootServicesData:
					return true;
				default:
					return false;
			}
		};

		eir::infoLogger() << "Memory map:" << frg::endlog;
		auto entry = nextEntry(0);

		while(entry) {
			auto lastContiguousEntry = entry;

			while(true) {
				auto next = nextEntry(endAddr(lastContiguousEntry));

				if(!next || !isContiguous(lastContiguousEntry, next))
					break;

				if(isUsable(lastContiguousEntry) != isUsable(next))
					break;

				lastContiguousEntry = next;
			}

			eir::infoLogger()
				<< "\tbase=0x" << frg::hex_fmt{entry->physical_start}
				<< " length=0x" << frg::hex_fmt{endAddr(lastContiguousEntry) - entry->physical_start}
				<< " usable=" << (isUsable(entry) ? "true" : "false")
				<< frg::endlog;

			if(isUsable(entry)) {
				createInitialRegions(
					{entry->physical_start, endAddr(lastContiguousEntry) - entry->physical_start},
					{reservedRegions.data(), nReservedRegions}
				);
			}

			entry = nextEntry(endAddr(lastContiguousEntry));
		}
	}
};

initgraph::Task setupAcpiInfo{&globalInitEngine,
	"uefi.setup-acpi-info",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		info_ptr->acpiRsdp = rsdp;
	}
};

initgraph::Task setupInitrdInfo{&globalInitEngine,
	"uefi.setup-initrd-info",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		auto initrd_module = bootAlloc<EirModule>(1);
		initrd_module->physicalBase = reinterpret_cast<EirPtr>(initrd);
		initrd_module->length = initrdInfo->file_size;
		const char *initrd_mod_name = "initrd.cpio";
		size_t name_length = strlen(initrd_mod_name);
		char *name_ptr = bootAlloc<char>(name_length);
		memcpy(name_ptr, initrd_mod_name, name_length);
		initrd_module->namePtr = mapBootstrapData(name_ptr);
		initrd_module->nameLength = name_length;

		info_ptr->moduleInfo = mapBootstrapData(initrd_module);
	}
};

initgraph::Task mapEirImage{&globalInitEngine,
	"uefi.map-eir-image",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		auto base = reinterpret_cast<uintptr_t>(loadedImage->image_base);
		auto pages = (loadedImage->image_size >> 12) + 1;

		for(size_t i = 0; i < pages; i++) {
			mapSingle4kPage(base + (i << 12), base + (i << 12), PageFlags::write | PageFlags::execute);
		}
	}
};

initgraph::Task setupFramebufferInfo{&globalInitEngine,
	"uefi.setup-framebuffer-info",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		if(gop && gop->mode->info->pixel_format != PixelBltOnly) {
			fb = &info_ptr->frameBuffer;

			switch(gop->mode->info->pixel_format) {
				case PixelBlueGreenRedReserved8BitPerColor:
					fb->fbBpp = 32;
					break;
				case PixelRedGreenBlueReserved8BitPerColor:
					fb->fbBpp = 32;
					break;
				case PixelBitMask: {
					assert(gop->mode->info->pixel_information.red_mask);
					assert(gop->mode->info->pixel_information.green_mask);
					assert(gop->mode->info->pixel_information.blue_mask);

					size_t hbred = (sizeof(uint32_t) * 8) - __builtin_clz(gop->mode->info->pixel_information.red_mask);
					size_t hbgreen = (sizeof(uint32_t) * 8) - __builtin_clz(gop->mode->info->pixel_information.green_mask);
					size_t hbblue = (sizeof(uint32_t) * 8) - __builtin_clz(gop->mode->info->pixel_information.blue_mask);

					frg::optional<size_t> hbres = {};
					if(gop->mode->info->pixel_information.reserved_mask)
						hbres = (sizeof(uint32_t) * 8) - __builtin_clz(gop->mode->info->pixel_information.reserved_mask);

					size_t highest_bit = frg::max(hbred, hbgreen);
					highest_bit = frg::max(highest_bit, hbblue);
					if(hbres)
						highest_bit = frg::max(highest_bit, hbres.value());

					assert(highest_bit % 8 == 0);
					fb->fbBpp = highest_bit;
					break;
				}
				default:
					eir::panicLogger() << "eir: unhandled GOP pixel format" << frg::endlog;
			}

			fb->fbAddress = gop->mode->framebuffer_base;
			fb->fbPitch = gop->mode->info->pixels_per_scan_line * (fb->fbBpp / 8);
			fb->fbWidth = gop->mode->info->horizontal_resolution;
			fb->fbHeight = gop->mode->info->vertical_resolution;
			fb->fbType = 1; // linear framebuffer
		}
	}
};

} // namespace

extern "C" efi_status eirUefiMain(const efi_handle h, const efi_system_table *system_table) {
	eirRunConstructors();

	// Set the system table so we can use loggers early on.
	st = system_table;
	bs = st->boot_services;
	handle = h;

	logHandler = uefiBootServicesLogHandler;

	// Reset the watchdog timer.
	EFI_CHECK(bs->set_watchdog_timer(0, 0, 0, nullptr));
	EFI_CHECK(st->con_out->clear_screen(st->con_out));

	// Get a handle to this binary in order to get the command line.
	efi_guid protocol = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_CHECK(bs->handle_protocol(handle, &protocol, reinterpret_cast<void **>(&loadedImage)));

	// Convert the command line to ASCII.
	char *ascii_cmdline = nullptr;
	{
		EFI_CHECK(bs->allocate_pool(EfiLoaderData,
			(loadedImage->load_options_size / sizeof(uint16_t)) + 1,
			reinterpret_cast<void **>(&ascii_cmdline)));
		size_t i = 0;
		for(; i < loadedImage->load_options_size / sizeof(char16_t); i++) {
			auto c = reinterpret_cast<char16_t *>(loadedImage->load_options)[i];
			// we only use printable ASCII characters, everything else gets discarded
			if(c >= 0x20 && c <= 0x7E) {
				ascii_cmdline[i] = c;
			} else {
				ascii_cmdline[i] = '\0';
			}
		}
		// Null-terminate the buffer.
		ascii_cmdline[i] = '\0';
	}
	assert(ascii_cmdline);
	cmdline = {ascii_cmdline, strlen(ascii_cmdline)};

	bool eir_gdb_ready_val = true;

	frg::array args = {
		// allow for attaching GDB to eir
		frg::option{"eir.efidebug", frg::store_false(eir_gdb_ready_val)},
		frg::option{"bochs", frg::store_true(log_e9)},
		frg::option{"eir.initrd", frg::as_string_view(initrd_path)},
	};
	frg::parse_arguments(ascii_cmdline, args);

	eir::infoLogger() << "eir: command line='" << ascii_cmdline << "'" << frg::endlog;

	// this needs to be volatile as GDB sets this to true on attach
	volatile bool eir_gdb_ready = eir_gdb_ready_val;

	if(!eir_gdb_ready_val) {
#ifdef __x86_64__
		// exfiltrate our base address for use with gdb
		constexpr arch::scalar_register<uint8_t> offset{0};
		auto port = arch::global_io.subspace(0xCB7);

		for(size_t i = 0; i < sizeof(uintptr_t); i++) {
			uint8_t b = reinterpret_cast<uintptr_t>(loadedImage->image_base) >> (i * 8);
			port.store(offset, b);
		}
#endif

		eir::infoLogger() << "eir: image base address " << frg::hex_fmt{loadedImage->image_base} << frg::endlog;
		eir::infoLogger() << "eir: Waiting for GDB to attach" << frg::endlog;
	}

	while(!eir_gdb_ready);

	eirMain();

	return EFI_SUCCESS;
}

} // namespace eir
