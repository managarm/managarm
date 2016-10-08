
#ifndef FRIGG_MACHINE_HPP
#define FRIGG_MACHINE_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>
#include <frigg/array.hpp>

namespace frigg FRIGG_VISIBILITY {
namespace arch_x86 {

enum {
	kCpuIndexStructuredExtendedFeaturesEnum = 7,
	kCpuIndexExtendedFeatures = 0x80000001
};

enum {
	// Structured extended features enumeration, EBX register
	kCpuFlagFsGsBase = 1,

	// Extendend features, EDX register
	kCpuFlagSyscall = 0x800,
	kCpuFlagNx = 0x100000,
	kCpuFlagLongMode = 0x20000000
};

inline Array<uint32_t, 4> cpuid(uint32_t eax, uint32_t ecx = 0) {
	Array<uint32_t, 4> out;
	asm volatile ( "cpuid"
			: "=a" (out[0]), "=b" (out[1]), "=c" (out[2]), "=d" (out[3])
			: "a" (eax), "c" (ecx) );
	return out;
}

enum {
	kMsrLocalApicBase = 0x0000001B,
	kMsrEfer = 0xC0000080,
	kMsrStar = 0xC0000081,
	kMsrLstar = 0xC0000082,
	kMsrFmask = 0xC0000084,
	kMsrIndexFsBase = 0xC0000100,
	kMsrIndexGsBase = 0xC0000101,
	kMsrIndexKernelGsBase = 0xC0000102
};

enum {
	kMsrSyscallEnable = 1
};

inline void wrmsr(uint32_t index, uint64_t value) {
	uint32_t low = value;
	uint32_t high = value >> 32;
	asm volatile ( "wrmsr" : : "c" (index),
			"a" (low), "d" (high) : "memory" );
}

inline uint64_t rdmsr(uint32_t index) {
	uint32_t low, high;
	asm volatile ( "rdmsr" : "=a" (low), "=d" (high)
			: "c" (index) : "memory" );
	return ((uint64_t)high << 32) | (uint64_t)low;
}

inline uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

inline uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile ( "inw %%dx, %%ax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

inline void ioPeekMultiple(uint16_t port, uint16_t *dest, size_t count) {
	asm volatile ( "cld\n"
			"\trep insw" : : "d" (port), "D" (dest), "c" (count) );
}

inline void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

} } // namespace frigg::arch_x86

namespace frigg FRIGG_VISIBILITY {

template<typename T>
T readIo(uint16_t port);

template<typename T>
void writeIo(uint16_t port, T value);

template<>
inline uint8_t readIo<uint8_t>(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "in %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

template<>
inline uint16_t readIo<uint16_t>(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile ( "in %%dx, %%ax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

template<>
inline uint32_t readIo<uint32_t>(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint32_t out_value asm("eax");
	asm volatile ( "in %%dx, %%eax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

template<>
inline void writeIo<uint8_t>(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "out %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

template<>
inline void writeIo<uint16_t>(uint16_t port, uint16_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t in_value asm("ax") = value;
	asm volatile ( "out %%ax, %%dx" : : "r" (in_port), "r" (in_value) );
}

template<>
inline void writeIo<uint32_t>(uint16_t port, uint32_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint32_t in_value asm("eax") = value;
	asm volatile ( "out %%eax, %%dx" : : "r" (in_port), "r" (in_value) );
}

} // namespace frigg

#endif // FRIGG_MACHINE_HPP
