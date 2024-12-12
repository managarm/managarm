#pragma once

#include <stdint.h>

namespace sbi {

using Error = int64_t;

struct Result {
	int64_t error;
	int64_t value;
};

constexpr int64_t eidBase = 0x10;
constexpr int64_t eidTime = 0x54494D45;
constexpr int64_t eidIpi = 0x735049;
constexpr int64_t eidHsm = 0x48534D;
constexpr int64_t eidDbcn = 0x4442434E;

inline Result call1(uint64_t eid, uint64_t fid, uint64_t arg0) {
	register uint64_t rEid asm("a7") = eid;
	register uint64_t rFid asm("a6") = fid;
	register uint64_t a0 asm("a0") = arg0;
	register uint64_t a1 asm("a1");
	asm volatile("ecall" : "+r"(a0), "=r"(a1) : "r"(rEid), "r"(rFid) : "memory");
	return Result{static_cast<int64_t>(a0), static_cast<int64_t>(a1)};
}

inline Result call2(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1) {
	register uint64_t rEid asm("a7") = eid;
	register uint64_t rFid asm("a6") = fid;
	register uint64_t a0 asm("a0") = arg0;
	register uint64_t a1 asm("a1") = arg1;
	asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(rEid), "r"(rFid) : "memory");
	return Result{static_cast<int64_t>(a0), static_cast<int64_t>(a1)};
}

inline Result call3(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
	register uint64_t rEid asm("a7") = eid;
	register uint64_t rFid asm("a6") = fid;
	register uint64_t a0 asm("a0") = arg0;
	register uint64_t a1 asm("a1") = arg1;
	register uint64_t a2 asm("a2") = arg2;
	asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(rEid), "r"(rFid) : "memory");
	return Result{static_cast<int64_t>(a0), static_cast<int64_t>(a1)};
}

} // namespace sbi

namespace sbi::base {

constexpr int64_t fidProbeExtension = 3;

inline int64_t probeExtension(int64_t eid) {
	auto res = call1(eidBase, fidProbeExtension, eid);
	if (res.error)
		__builtin_trap();
	return res.value;
}

} // namespace sbi::base

namespace sbi::time {

constexpr int64_t fidSetTimer = 0;

inline Error setTimer(uint64_t deadline) {
	auto res = call1(eidTime, fidSetTimer, deadline);
	return res.error;
}

} // namespace sbi::time

namespace sbi::ipi {

constexpr int64_t fidSendIpi = 0;

inline Error sendIpi(uint64_t hartMask, uint64_t hartMaskBase) {
	auto res = call2(eidIpi, fidSendIpi, hartMask, hartMaskBase);
	return res.error;
}

} // namespace sbi::ipi

namespace sbi::hsm {

constexpr int64_t fidHartStart = 0;

inline Error hartStart(uint64_t hartId, uint64_t ip, uint64_t a1) {
	auto res = call3(eidHsm, fidHartStart, hartId, ip, a1);
	return res.error;
}

} // namespace sbi::hsm

namespace sbi::dbcn {

constexpr int64_t fidWrite = 0;
constexpr int64_t fidRead = 1;
constexpr int64_t fidWriteByte = 2;

inline Error writeByte(uint8_t b) {
	auto res = call1(eidDbcn, fidWriteByte, b);
	return res.error;
}

inline Error writeString(const char *s) {
	while (*s) {
		if (auto e = writeByte(*s); e)
			return e;
		++s;
	}
	return 0;
}

} // namespace sbi::dbcn
