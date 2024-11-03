#include <arch/io_space.hpp>
#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <frg/array.hpp>
#include <frg/cmdline.hpp>
#include <stdbool.h>

#include "efi.hpp"
#include "helpers.hpp"

static_assert(sizeof(char16_t) == sizeof(wchar_t), "Strings are not UTF-16-ish, are you missing -fshort-wchar?");
extern "C" void eirEnterKernel(uintptr_t, uint64_t, uint64_t) __attribute__((sysv_abi));

namespace eir {

const efi_system_table *st = nullptr;
const efi_boot_services *bs = nullptr;
efi_handle handle = nullptr;

namespace {

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

} // namespace

extern "C" efi_status eirUefiMain(const efi_handle h, const efi_system_table *system_table) {
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
	efi_loaded_image_protocol *loadedImage = nullptr;
	EFI_CHECK(bs->handle_protocol(handle, &protocol, reinterpret_cast<void **>(&loadedImage)));

	// Convert the command line to ASCII.
	char *cmdLine = nullptr;
	{
		EFI_CHECK(bs->allocate_pool(EfiLoaderData,
			(loadedImage->load_options_size / sizeof(uint16_t)) + 1,
			reinterpret_cast<void **>(&cmdLine)));
		size_t i = 0;
		for(; i < loadedImage->load_options_size / sizeof(char16_t); i++) {
			auto c = reinterpret_cast<char16_t *>(loadedImage->load_options)[i];
			// we only use printable ASCII characters, everything else gets discarded
			if(c >= 0x20 && c <= 0x7E) {
				cmdLine[i] = c;
			} else {
				cmdLine[i] = '\0';
			}
		}
		// Null-terminate the buffer.
		cmdLine[i] = '\0';
	}
	assert(cmdLine);

	bool eir_gdb_ready_val = true;
	frg::string_view initrd_path = "managarm\\initrd.cpio";

	frg::array args = {
		// allow for attaching GDB to eir
		frg::option{"eir.efidebug", frg::store_false(eir_gdb_ready_val)},
		frg::option{"bochs", frg::store_true(log_e9)},
		frg::option{"eir.initrd", frg::as_string_view(initrd_path)},
	};
	frg::parse_arguments(cmdLine, args);

	eir::infoLogger() << "eir: command line='" << cmdLine << "'" << frg::endlog;

	// this needs to be volatile as GDB sets this to true on attach
	volatile bool eir_gdb_ready = eir_gdb_ready_val;

	if(!eir_gdb_ready_val) {
		// exfiltrate our base address for use with gdb
		constexpr arch::scalar_register<uint8_t> offset{0};
		auto port = arch::global_io.subspace(0xCB7);

		for(size_t i = 0; i < sizeof(uintptr_t); i++) {
			uint8_t b = reinterpret_cast<uintptr_t>(loadedImage->image_base) >> (i * 8);
			port.store(offset, b);
		}

		eir::infoLogger() << "eir: Waiting for GDB to attach" << frg::endlog;
	}

	while(!eir_gdb_ready);

	// acquire ACPI table info
	efi_guid acpi_guid = ACPI_20_TABLE_GUID;
	const efi_configuration_table *t = st->configuration_table;
	uintptr_t rsdp = 0;
	for(size_t i = 0; i < st->number_of_table_entries && t; i++, t++)
		if(!memcmp(&acpi_guid, &t->vendor_guid, sizeof(acpi_guid)))
			rsdp = reinterpret_cast<uintptr_t>(t->vendor_table);

	efi_file_protocol *initrdFile = nullptr;
	EFI_CHECK(fsOpen(&initrdFile, asciiToUcs2(initrd_path)));
	efi_file_info *initrdInfo = fsGetInfo(initrdFile);

	// Read initrd.
	efi_physical_addr initrd = 0;
	EFI_CHECK(bs->allocate_pages(AllocateAnyPages, EfiLoaderData, (initrdInfo->file_size >> 12) + 1, &initrd));
	EFI_CHECK(fsRead(initrdFile, initrdInfo->file_size, 0, initrd));

	parseInitrd(reinterpret_cast<void *>(initrd));

	// Get the frame buffer.
	efi_graphics_output_protocol *gop = nullptr;

	{
		efi_guid gop_protocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
		EFI_CHECK(bs->locate_protocol(&gop_protocol, nullptr, reinterpret_cast<void **>(&gop)));
		if(gop->mode->info->version != 0)
			eir::panicLogger() << "error: unsupported EFI_GRAPHICS_OUTPUT_MODE_INFORMATION version!" << frg::endlog;

		eir::infoLogger() << "eir: framebuffer "
			<< gop->mode->info->horizontal_resolution << "x"
			<< gop->mode->info->vertical_resolution << " address=0x"
			<< frg::hex_fmt{gop->mode->framebuffer_base}
			<< frg::endlog;
	}

	size_t memMapSize = sizeof(efi_memory_descriptor);
	size_t mapKey = 0;
	size_t descriptorSize = 0;
	uint32_t descriptorVersion = 0;
	void *memMap = nullptr;
	efi_memory_descriptor dummy;

	// First get the size of the memory map buffer to allocate.
	efi_status status = bs->get_memory_map(&memMapSize, &dummy, &mapKey, &descriptorSize, &descriptorVersion);
	assert(status == EFI_BUFFER_TOO_SMALL);

	// over-allocate a bit to accomodate the allocation we have to make here
	// we only get one shot(tm) to allocate an appropriately-sized buffer, as the spec does not
	// allow for calling any boot services other than GetMemoryMap and ExitBootServices after
	// a call to ExitBootServices fails
	memMapSize += 8 * descriptorSize;
	EFI_CHECK(bs->allocate_pool(EfiLoaderData, memMapSize, &memMap));

	// Now, get the actual memory map.
	EFI_CHECK(bs->get_memory_map(&memMapSize,
		reinterpret_cast<efi_memory_descriptor*>(memMap),
		&mapKey, &descriptorSize, &descriptorVersion));

	// Exit boot services.
	EFI_CHECK(bs->exit_boot_services(handle, mapKey));
	bs = nullptr;
#if defined(__x86_64__)
	asm volatile ("cli");
#else
#error "Unsupported architecture!"
#endif

	frg::array<InitialRegion, 4> reservedRegions = {};
	size_t nReservedRegions = 0;

	reservedRegions[nReservedRegions++] = {reinterpret_cast<uintptr_t>(loadedImage->image_base), loadedImage->image_size};
	reservedRegions[nReservedRegions++] = {initrd, initrdInfo->file_size};

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

	initProcessorEarly();
	setupRegionStructs();

	uint64_t kernel_entry = 0;
	initProcessorPaging(reinterpret_cast<void *>(kernel_image.data()), kernel_entry);

	EirInfo *info_ptr = generateInfo(cmdLine);

	auto initrd_module = bootAlloc<EirModule>(1);
	initrd_module->physicalBase = reinterpret_cast<EirPtr>(initrd);
	initrd_module->length = initrdInfo->file_size;
	const char *initrd_mod_name = "initrd.cpio";
	size_t name_length = strlen(initrd_mod_name);
	char *name_ptr = bootAlloc<char>(name_length);
	memcpy(name_ptr, initrd_mod_name, name_length);
	initrd_module->namePtr = mapBootstrapData(name_ptr);
	initrd_module->nameLength = name_length;

	info_ptr->numModules = 1;
	info_ptr->moduleInfo = mapBootstrapData(initrd_module);

	info_ptr->acpiRsdp = rsdp;

	auto base = reinterpret_cast<uintptr_t>(loadedImage->image_base);
	auto pages = (loadedImage->image_size >> 12) + 1;

	for(size_t i = 0; i < pages; i++) {
		mapSingle4kPage(base + (i << 12), base + (i << 12), PageFlags::write | PageFlags::execute);
	}

	if(gop && gop->mode->info->pixel_format != PixelBltOnly) {
		auto fb = &info_ptr->frameBuffer;

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

		// Map the framebuffer.
		assert(fb->fbAddress & ~static_cast<EirPtr>(pageSize - 1));
		for(address_t pg = 0; pg < gop->mode->framebuffer_size; pg += 0x1000)
			mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, fb->fbAddress + pg,
					PageFlags::write, CachingMode::writeCombine);
		mapKasanShadow(0xFFFF'FE00'4000'0000, fb->fbPitch * fb->fbHeight);
		unpoisonKasanShadow(0xFFFF'FE00'4000'0000, fb->fbPitch * fb->fbHeight);
		fb->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	}

	// Hand-off to thor
	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eirEnterKernel(eirPml4Pointer, kernel_entry, 0xFFFF'FE80'0001'0000); // TODO

	return EFI_SUCCESS;
}

} // namespace eir
