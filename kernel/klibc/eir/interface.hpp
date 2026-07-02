#pragma once

#include <array>
#include <frg/string.hpp>
#include <stdint.h>

static const uint32_t eirDebugSerial = 1;
static const uint32_t eirDebugBochs = 2;
static const uint32_t eirDebugKernelProfile = 16;

typedef uint64_t EirPtr;
typedef uint64_t EirSize;

static const EirSize eirMaxMemoryRegions = 64;

struct EirRegion {
	EirPtr address;
	EirSize length;
	EirSize order; // TODO: This could be an int.
	EirSize numRoots;
	EirPtr buddyTree;
};

struct EirFramebuffer {
	EirPtr fbAddress;
	EirPtr fbEarlyWindow;
	EirSize fbPitch;
	EirSize fbWidth;
	EirSize fbHeight;
	EirSize fbBpp;
	EirSize fbType;
};

// Please keep this sorted.
enum class RiscvExtension {
	a,
	c,
	d,
	f,
	h,
	i,
	m,
	// S extensions.
	ssdbltrp,
	sstc,
	svadu,
	svpbmt,
	// Z extensions.
	za64rs,
	zic64b,
	zicbom,
	zicbop,
	zicboz,
	ziccamoa,
	ziccif,
	zicclsm,
	ziccrse,
	zicntr,
	zicsr,
	zifencei,
	zihintpause,
	zihpm,

	// Number of features. Must be last.
	numExtensions,
	invalid = numExtensions,
};

#define EIR_STRINGIFY_RISCV_EXTENSION_CASE(ext) case RiscvExtension::ext: return #ext;

#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
constexpr const char *stringifyRiscvExtension(RiscvExtension ext) {
	switch(ext) {
		// Please keep this sorted.
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(a)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(c)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(d)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(f)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(h)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(i)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(m)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(ssdbltrp)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(sstc)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(svadu)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(svpbmt)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(za64rs)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zic64b)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicbom)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicbop)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicboz)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(ziccamoa)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(ziccif)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicclsm)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(ziccrse)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicntr)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zicsr)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zifencei)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zihintpause)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(zihpm)
		case RiscvExtension::invalid:
			break;
	}
	return nullptr;
};
#pragma GCC diagnostic pop

constexpr auto riscvExtensionStrings = [] {
	constexpr size_t n = static_cast<unsigned int>(RiscvExtension::numExtensions);
	std::array<const char *, n> array;
	for (size_t i = 0; i < n; ++i)
		array[i] = stringifyRiscvExtension(static_cast<RiscvExtension>(i));
	return array;
}();

inline RiscvExtension parseRiscvExtension(frg::string_view s) {
	constexpr size_t n = static_cast<unsigned int>(RiscvExtension::numExtensions);
	for (size_t i = 0; i < n; ++i) {
		if (riscvExtensionStrings[i] == s)
			return static_cast<RiscvExtension>(i);
	}
	return RiscvExtension::invalid;
}

const int extensionBitmaskWords = 1;
static_assert(static_cast<unsigned int>(RiscvExtension::numExtensions) < extensionBitmaskWords * 64);

namespace elf_note_type {

// Values for Elf64_Nhdr::n_type of ELF notes embedded into Thor.
// 0x10xx'xxxx range reserved for generic configuration notes in Thor (write-only by Eir).
constexpr unsigned int memoryLayout = 0x1000'0000;
constexpr unsigned int cpuConfig = 0x1000'0001;
constexpr unsigned int smbiosData = 0x1000'0002;
constexpr unsigned int bootUartConfig = 0x1000'0003;
constexpr unsigned int debugOptions = 0x1000'0004;
constexpr unsigned int acpiData = 0x1000'0005;
constexpr unsigned int dtData = 0x1000'0006;
constexpr unsigned int framebuffer = 0x1000'0007;
constexpr unsigned int initrd = 0x1000'0008;
constexpr unsigned int physicalMemory = 0x1000'0009;
constexpr unsigned int commandLine = 0x1000'000a;
// 0x11xx'xxxx range reserved for arch-specific configuration notes in Thor (write-only by Eir).
// 0x1100'0xxx range reserved for x86.
// 0x1100'1xxx range reserved for aarch64.
// 0x1100'2xxx range reserved for riscv64.
constexpr unsigned int riscvConfig = 0x1100'2000;
constexpr unsigned int riscvHartCaps = 0x1100'2001;
// 0x18xx'xxxx range reserved for generic capability notes in Thor (read-only by Eir).
constexpr unsigned int perCpuRegion = 0x1800'0000;
constexpr unsigned int debugCapabilities = 0x1800'0001;

inline bool isThorConfiguration(unsigned int type) {
	return (type & 0xF800'0000) == 0x1000'0000;
}

inline bool isThorCapability(unsigned int type) {
	return (type & 0xF800'0000) == 0x1800'0000;
}

inline bool isThorGenericConfiguration(unsigned int type) {
	return (type & 0xFF00'0000) == 0x1000'0000;
}

inline bool isThorArchSpecificConfiguration(unsigned int type) {
	return (type & 0xFF00'0000) == 0x1100'0000;
}

inline bool isThorGenericCapability(unsigned int type) {
	return (type & 0xFF00'0000) == 0x1800'0000;
}

} // namespace elf_note_type

struct MemoryLayout {
	// Address of the direct physical mapping.
	uint64_t directPhysical;
	// Address and size of the kernel virtual mapping area.
	uint64_t kernelVirtual;
	uint64_t kernelVirtualSize;
	// Address and size of the allocation log ring buffer.
	uint64_t allocLog;
	uint64_t allocLogSize;
	// Base address of the bootstrap data area: a region of Thor's virtual address
	// space into which Eir maps variable-length boot information (e.g., the kernel
	// command line and the physical memory map) before handing off to Thor.
	uint64_t bootstrapData;
};

struct RiscvConfig {
	// Number of levels of page tables.
	// 3 for Sv39,
	// 4 for Sv48,
	// 5 for Sv57.
	int32_t numPtLevels{0};
	uint32_t pad{0};
	uint64_t bspHartId{0};
};

struct RiscvHartCaps {
	uint64_t extensions[extensionBitmaskWords]{};

	void setExtension(RiscvExtension ext) {
		auto n = static_cast<unsigned int>(ext);
		extensions[n >> 6] |= (UINT64_C(1) << (n & 63));
	}
	bool hasExtension(RiscvExtension ext) {
		auto n = static_cast<unsigned int>(ext);
		return extensions[n >> 6] & (UINT64_C(1) << (n & 63));
	}
};

struct PerCpuRegion {
	uint64_t start;
	uint64_t end;
};

struct CpuConfig {
	uint64_t effectiveCpus;
	uint64_t totalCpus;
};

struct SmbiosData {
	EirPtr address;
};

enum class BootUartType {
	none,
	pl011,
	samsung,
};

struct BootUartConfig {
	uint64_t address = 0;
	uint64_t size = 0;
	uint64_t window = 0;
	BootUartType type = BootUartType::none;
};

struct DebugOptions {
	uint32_t flags = 0;
	bool ubsanAbort = true;
	// Use ACPI + PCI in userspace.
	// Not really a debug option but expected to be temporary (thus we do not add a new ELF note).
	bool useSif = false;
};

struct DebugCapabilities {
	bool kasan = false;
};

struct AcpiData {
	uint64_t rsdp = 0;
};

struct DtData {
	uint64_t address = 0;
	uint64_t size = 0;
};

struct Initrd {
	uint64_t physicalBase = 0;
	uint64_t length = 0;
};

struct PhysicalMemory {
	// Number of regions in the array pointed to by regionInfo.
	uint64_t numRegions = 0;
	// Virtual address of an EirRegion[numRegions] array in the bootstrap data area.
	uint64_t regionInfo = 0;
};

struct CommandLine {
	// Virtual address of a null-terminated string in the bootstrap data area.
	uint64_t ptr = 0;
};
