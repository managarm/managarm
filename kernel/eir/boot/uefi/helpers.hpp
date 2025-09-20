#pragma once

#include <assert.h>
#include <eir-internal/debug.hpp>
#include <frg/logging.hpp>
#include <frg/string.hpp>
#include <source_location>

#include "efi.hpp"

void EFI_CHECK(efi_status s, std::source_location loc = std::source_location::current());

namespace eir {

extern const efi_system_table *st;
extern const efi_boot_services *bs;
extern efi_handle handle;

efi_status fsOpen(efi_file_protocol **file, char16_t *path);
efi_status fsRead(efi_file_protocol *file, size_t len, size_t offset, efi_physical_addr buf);
size_t fsGetSize(efi_file_protocol *file);

char16_t *asciiToUcs2(frg::string_view &s);

} // namespace eir
