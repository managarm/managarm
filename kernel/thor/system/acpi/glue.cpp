#include <limits.h>

#include <frg/cmdline.hpp>
#include <frg/string.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>

#include <uacpi/context.h>
#include <uacpi/kernel_api.h>

using namespace thor;

void uacpi_kernel_log(enum uacpi_log_level lvl, const char *msg) {
	const char *lvlStr;

	switch (lvl) {
		case UACPI_LOG_DEBUG:
			lvlStr = "debug";
			break;
		case UACPI_LOG_TRACE:
			lvlStr = "trace";
			break;
		case UACPI_LOG_INFO:
			lvlStr = "info";
			break;
		case UACPI_LOG_WARN:
			lvlStr = "warn";
			break;
		case UACPI_LOG_ERROR:
			lvlStr = "error";
			break;
		default:
			lvlStr = "<invalid>";
	}

	auto msgView = frg::string_view(msg);
	if (msgView.ends_with("\n"))
		msgView = msgView.sub_string(0, msgView.size() - 1);

	infoLogger() << "uacpi-" << lvlStr << ": " << msgView << frg::endlog;
}

namespace {

constexpr struct {
	frg::string_view name;
	uacpi_log_level level;
} uacpiLogLevels[] = {
    {"error", UACPI_LOG_ERROR},
    {"warn", UACPI_LOG_WARN},
    {"info", UACPI_LOG_INFO},
    {"trace", UACPI_LOG_TRACE},
    {"debug", UACPI_LOG_DEBUG},
};

} // namespace

void thor::acpi::configureLogLevel() {
	frg::string_view name;

	frg::array args = {
	    frg::option{"uacpi.log", frg::as_string_view(name)},
	};
	frg::parse_arguments(getKernelCmdline(), args);

	if (!name.size())
		return;

	for (auto entry : uacpiLogLevels) {
		if (entry.name != name)
			continue;
		uacpi_context_set_log_level(entry.level);
		return;
	}

	infoLogger() << "thor: Ignoring unknown uacpi.log level \"" << name << "\"" << frg::endlog;
}

// TODO: We do not want to keep things mapped forever.
void *uacpi_kernel_map(uacpi_phys_addr physical, uacpi_size length) {
	auto pow2ceil = [](unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto paddr = physical & ~(kPageSize - 1);
	auto vsize = length + (physical & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	auto caching = determineFirmwareCachingMode(physical, length);
	if (!caching)
		return nullptr;

	auto ptr = KernelVirtualMemory::global().allocate(msize);
	for (size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(
		    (VirtualAddr)ptr + pg, paddr + pg, page_access::write, *caching
		);
	return reinterpret_cast<char *>(ptr) + (physical & (kPageSize - 1));
}

void uacpi_kernel_unmap(void *ptr, uacpi_size length) {
	auto pow2ceil = [](unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto vaddr = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
	auto vsize = length + (reinterpret_cast<uintptr_t>(ptr) & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	for (size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().unmapSingle4k(vaddr + pg);
	// TODO: free the virtual memory range.
	(void)msize;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
	*out_rsdp_address = thor::acpiRsdpNote->rsdp;
	return UACPI_STATUS_OK;
}

namespace thor {

THOR_DEFINE_ELF_NOTE(acpiRsdpNote){elf_note_type::acpiData, {}};

} // namespace thor
