
namespace frigg {
namespace arch_x86 {

enum {
	kCpuIndexExtendedFeatures = 0x80000001
};
enum {
	// Extendend features, EDX register
	kCpuFlagNx = 0x100000,
	kCpuFlagLongMode = 0x20000000
};

extern inline util::Array<uint32_t, 4> cpuid(uint32_t eax, uint32_t ecx = 0) {
	util::Array<uint32_t, 4> out;
	asm volatile ( "cpuid"
			: "=a" (out[0]), "=b" (out[1]), "=c" (out[2]), "=d" (out[3])
			: "a" (eax), "c" (ecx) );
	return out;
}

} } // namespace frigg::arch_x86

