#pragma once

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

namespace elf_note_type {

// Values for Elf64_Nhdr::n_type of ELF notes embedded into Thor.
constexpr unsigned int memoryLayout = 0x1000'0000;

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
