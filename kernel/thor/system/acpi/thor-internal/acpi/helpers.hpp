#pragma once

#include <frg/optional.hpp>
#include <uacpi/types.h>

namespace thor::acpi {

frg::optional<uacpi_u64> intFromPackage(uacpi_object_array &pkg, size_t index) {
	if(pkg.count <= index)
		return frg::null_opt;

	uint64_t v;
	auto ret = uacpi_object_get_integer(pkg.objects[index], &v);
	if(ret != UACPI_STATUS_OK)
		return frg::null_opt;

	return v;
}

} // namespace thor::acpi
