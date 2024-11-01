#pragma once

#include <frg/optional.hpp>
#include <uacpi/types.h>

namespace thor::acpi {

frg::optional<uacpi_u64> intFromPackage(uacpi_package *pkg, size_t index) {
	if (pkg->count <= index)
		return frg::null_opt;

	if (pkg->objects[index]->type != UACPI_OBJECT_INTEGER)
		return frg::null_opt;

	return pkg->objects[index]->integer;
}

} // namespace thor::acpi
