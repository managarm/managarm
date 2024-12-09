#pragma once

#include <array>
#include <frg/string.hpp>
#include <stdint.h>

static const uint64_t eirSignatureValue = 0x68692C2074686F72;

static const uint32_t eirDebugSerial = 1;
static const uint32_t eirDebugBochs = 2;
static const uint32_t eirDebugKernelProfile = 16;

typedef uint64_t EirPtr;
typedef uint64_t EirSize;

struct EirRegion {
	EirPtr address;
	EirSize length;
	EirSize order; // TODO: This could be an int.
	EirSize numRoots;
	EirPtr buddyTree;
};

struct EirModule {
	EirPtr physicalBase;
	EirSize length;
	EirPtr namePtr;
	EirSize nameLength;
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

struct EirInfo {
	uint64_t signature;
	EirPtr commandLine;
	uint32_t debugFlags;
	uint32_t padding;

	uint64_t hartId;

	EirSize numRegions;
	EirPtr regionInfo;
	EirPtr moduleInfo;

	EirPtr dtbPtr;
	EirSize dtbSize;

	EirFramebuffer frameBuffer;

	uint64_t acpiRsdp;
};

// Please keep this sorted.
enum class RiscvExtension {
	a,
	c,
	d,
	f,
	i,
	m,
	// S extensions.
	sstc,
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
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(i)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(m)
		EIR_STRINGIFY_RISCV_EXTENSION_CASE(sstc)
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
// 0x10xx'xxxx range reserved for generic notes in Thor.
constexpr unsigned int memoryLayout = 0x1000'0000;
constexpr unsigned int perCpuRegion = 0x1000'0001;
// 0x11xx'xxxx range reserved for arch-specific notes in Thor.
// 0x1100'0xxx range reserved for x86.
// 0x1100'1xxx range reserved for aarch64.
// 0x1100'2xxx range reserved for riscv64.
constexpr unsigned int riscvConfig = 0x1100'2000;
constexpr unsigned int riscvHartCaps = 0x1100'2001;

inline bool isThorGeneric(unsigned int type) {
	return (type & 0xFF00'0000) == 0x1000'0000;
}

inline bool isThorArchSpecific(unsigned int type) {
	return (type & 0xFF00'0000) == 0x1100'0000;
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
	// Address of the EirInfo struct.
	uint64_t eirInfo;
};

struct RiscvConfig {
	// Number of levels of page tables.
	// 3 for Sv39,
	// 4 for Sv48,
	// 5 for Sv57.
	int numPtLevels{0};
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
