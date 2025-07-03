#pragma once

#include <core/smbios.hpp>
#include <frg/span.hpp>
#include <stddef.h>
#include <stdint.h>

size_t getSmbiosEntrySize(frg::span<uint8_t> smbiosTable, size_t offset);
frg::span<uint8_t> getSmbiosEntry(frg::span<uint8_t> smbiosTable, uint8_t type);
