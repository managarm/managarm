#include <string.h>

#include <frg/scope_exit.hpp>
#include <thor-internal/arch/kvm.hpp>
#include <thor-internal/physical.hpp>
#include <x86/machine.hpp>

namespace thor {

namespace {

constexpr uint32_t kvmCpuidSignature = 0x40000000;
constexpr uint32_t kvmCpuidFeatures = 0x40000001;

// Feature bits in EAX of kvmCpuidFeatures.
constexpr uint32_t kvmFeatureClocksource2 = uint32_t(1) << 3;
constexpr uint32_t kvmFeatureClocksourceStable = uint32_t(1) << 24;

constexpr uint32_t msrKvmSystemTimeNew = 0x4b564d01;

constexpr uint8_t pvclockTscStableBit = 1;

// struct pvclock_vcpu_time_info of the KVM ABI.
struct PvclockTimeInfo {
	uint32_t version;
	uint32_t pad0;
	uint64_t tscTimestamp;
	uint64_t systemTime;
	uint32_t tscToSystemMul;
	int8_t tscShift;
	uint8_t flags;
	uint8_t pad[2];
};
static_assert(sizeof(PvclockTimeInfo) == 32);

bool runningOnKvm() {
	if(!(common::x86::cpuid(common::x86::kCpuIndexFeatures)[2] & (uint32_t(1) << 31)))
		return false;

	auto leaf = common::x86::cpuid(kvmCpuidSignature);
	char vendor[12];
	memcpy(&vendor[0], &leaf[1], 4);
	memcpy(&vendor[4], &leaf[2], 4);
	memcpy(&vendor[8], &leaf[3], 4);
	if(memcmp(vendor, "KVMKVMKVM\0\0\0", 12) != 0)
		return false;
	return leaf[0] >= kvmCpuidFeatures;
}

} // anonymous namespace

std::optional<Pow2Fraction<Rounding::down>> getKvmclockTscTickDuration() {
	if(!runningOnKvm())
		return std::nullopt;
	auto features = common::x86::cpuid(kvmCpuidFeatures)[0];
	if(!(features & kvmFeatureClocksource2) || !(features & kvmFeatureClocksourceStable))
		return std::nullopt;

	auto physical = physicalAllocator->allocate(kPageSize);
	if(physical == static_cast<PhysicalAddr>(-1))
		return std::nullopt;
	frg::scope_exit freePage{[&] { physicalAllocator->free(physical, kPageSize); }};

	PageAccessor accessor{physical};
	auto info = reinterpret_cast<PvclockTimeInfo *>(accessor.get());
	memset(info, 0, sizeof(PvclockTimeInfo));

	// KVM updates the structure before it resumes the guest after the WRMSR.
	// We only need the scale; disable the updates again before the page is freed.
	common::x86::wrmsr(msrKvmSystemTimeNew, physical | 1);
	uint32_t version;
	uint32_t mul;
	int8_t shift;
	uint8_t flags;
	do {
		version = __atomic_load_n(&info->version, __ATOMIC_ACQUIRE);
		mul = __atomic_load_n(&info->tscToSystemMul, __ATOMIC_RELAXED);
		shift = __atomic_load_n(&info->tscShift, __ATOMIC_RELAXED);
		flags = __atomic_load_n(&info->flags, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
	} while((version & 1) || version != __atomic_load_n(&info->version, __ATOMIC_RELAXED));
	common::x86::wrmsr(msrKvmSystemTimeNew, 0);

	// Without the stable bit, the kvmclock scale may differ between vCPUs.
	if(!(flags & pvclockTscStableBit))
		return std::nullopt;
	// Other scales do not fit into the Pow2Fraction and its reciprocal (see computeReciprocal()).
	if(mul <= 1 || shift < -31 || shift > 31)
		return std::nullopt;

	// kvmclock computes ns = ((ticks << shift) * mul) >> 32, with a right shift for shift < 0.
	return Pow2Fraction<Rounding::down>{mul, 32 - shift};
}

} // namespace thor
