#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
#include <uacpi/kernel_api.h>

extern "C" {

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
	*out_rsdp_address = eir::eirRsdpAddr;
	return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size) { return eir::physToVirt<void *>(addr); }

void uacpi_kernel_unmap(void *, uacpi_size) {
	// No-op since we are using an identity mapping.
}

void uacpi_kernel_log(uacpi_log_level, const uacpi_char *msg) {
	frg::string_view sv{msg};
	if (sv.ends_with("\n"))
		sv = sv.sub_string(0, sv.size() - 1);
	eir::infoLogger() << "uacpi: " << sv << frg::endlog;
}

} // extern "C"
