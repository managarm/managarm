// Tests thor's x86 virtualization: exit completions, kicks and the in-kernel local APIC.
// The guest runs in real mode; it stores its results in guest memory, which the VMM checks
// at the end.

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <core/process-data.hpp>
#include <hel.h>
#include <hel-syscalls.h>

namespace {

// Guest-physical layout.
constexpr uint64_t memorySize = 0x10000;
constexpr uint64_t entry = 0x1000;
constexpr uint64_t results = 0x9000;

// Ports and MSRs that the guest uses to talk to the VMM.
constexpr uint16_t portDone = 0x501;
constexpr uint16_t portIn = 0x502;
constexpr uint16_t portAssert = 0x503;
constexpr uint32_t msrCompleted = 0x40000123;
constexpr uint32_t msrFaulted = 0x40000124;

// Offsets of the results (32-bit values).
enum : uint32_t {
	resCpuidEax = 0x00,
	resCpuidEbx = 0x04,
	resCpuidEcx = 0x08,
	resCpuidEdx = 0x0C,
	resIn = 0x10,
	resMsrLow = 0x14,
	resMsrHigh = 0x18,
	resGpCount = 0x1C,
	resApicId = 0x20,
	resApicVersion = 0x24,
	resTimerCount = 0x28,
	resIpiCount = 0x2C,
	resAssertCount = 0x30,
	resLevelCount = 0x34,
};

} // namespace

// The guest. Interrupt handlers are installed in the real-mode IVT; addresses are
// computed relative to the start of the blob (which the VMM copies to its entry point).
asm(R"(
.pushsection .data
.code16

.global guestCode
.global guestCodeEnd
guestCode:
	cli
	xor %ax, %ax
	mov %ax, %ds
	mov %ax, %ss
	mov $0x8000, %sp

	movw $(gpHandler - guestCode + 0x1000), 13 * 4
	movw $0, 13 * 4 + 2
	movw $(timerHandler - guestCode + 0x1000), 0x40 * 4
	movw $0, 0x40 * 4 + 2
	movw $(ipiHandler - guestCode + 0x1000), 0x41 * 4
	movw $0, 0x41 * 4 + 2
	movw $(assertHandler - guestCode + 0x1000), 0x42 * 4
	movw $0, 0x42 * 4 + 2
	movw $(levelHandler - guestCode + 0x1000), 0x43 * 4
	movw $0, 0x43 * 4 + 2

	// CPUID, completed by the VMM.
	mov $0x12345678, %eax
	mov $9, %ecx
	cpuid
	mov %eax, 0x9000
	mov %ebx, 0x9004
	mov %ecx, 0x9008
	mov %edx, 0x900C

	// IN, completed by the VMM.
	mov $0x502, %dx
	in %dx, %eax
	mov %eax, 0x9010

	// RDMSR, completed by the VMM.
	mov $0x40000123, %ecx
	rdmsr
	mov %eax, 0x9014
	mov %edx, 0x9018

	// RDMSR that the VMM faults (#GP).
	mov $0x40000124, %ecx
	rdmsr

	// x2APIC ID and version (handled by the kernel).
	mov $0x802, %ecx
	rdmsr
	mov %eax, 0x9020
	mov $0x803, %ecx
	rdmsr
	mov %eax, 0x9024

	// Reading EOI (write-only) raises #GP in the kernel.
	mov $0x80B, %ecx
	rdmsr

	// The software-disabled local APIC drops interrupts (here: through SELF_IPI).
	mov $0x83F, %ecx
	mov $0x41, %eax
	xor %edx, %edx
	wrmsr

	// Enable the local APIC (SVR).
	mov $0x80F, %ecx
	mov $0x1FF, %eax
	xor %edx, %edx
	wrmsr

	// One-shot timer: divide by 1, vector 0x40, 1 ms.
	mov $0x83E, %ecx
	mov $0xB, %eax
	wrmsr
	mov $0x832, %ecx
	mov $0x40, %eax
	wrmsr
	mov $0x838, %ecx
	mov $1000000, %eax
	wrmsr
1:	cli
	cmpl $1, 0x9028
	jae 2f
	sti
	hlt
	jmp 1b
2:
	// TSC-deadline timer: 1M TSC ticks from now.
	mov $0x832, %ecx
	mov $0x40040, %eax
	xor %edx, %edx
	wrmsr
	rdtsc
	add $1000000, %eax
	adc $0, %edx
	mov $0x6E0, %ecx
	wrmsr
1:	cli
	cmpl $2, 0x9028
	jae 2f
	sti
	hlt
	jmp 1b
2:
	// IPIs to ourselves: vector 0x41 to physical destination 0, then through the
	// self shorthand. An IPI to all others reaches nobody.
	mov $0x830, %ecx
	xor %edx, %edx
	mov $0x0041, %eax
	wrmsr
1:	cli
	cmpl $1, 0x902C
	jae 2f
	sti
	hlt
	jmp 1b
2:
	mov $0x40041, %eax
	wrmsr
1:	cli
	cmpl $2, 0x902C
	jae 2f
	sti
	hlt
	jmp 1b
2:
	mov $0xC0041, %eax
	wrmsr
	// The VMM raises vector 0x42 (edge-triggered).
	mov $0x503, %dx
	mov $0x42, %al
	out %al, %dx
1:	cli
	cmpl $1, 0x9030
	jae 2f
	sti
	hlt
	jmp 1b
2:
	// The VMM raises vector 0x43 (level-triggered); our EOI exits to the VMM.
	mov $0x503, %dx
	mov $0x43, %al
	out %al, %dx
1:	cli
	cmpl $1, 0x9034
	jae 2f
	sti
	hlt
	jmp 1b
2:
	mov $0x501, %dx
	mov $1, %al
	out %al, %dx
3:	cli
	hlt
	jmp 3b

eoi:
	pushal
	mov $0x80B, %ecx
	xor %eax, %eax
	xor %edx, %edx
	wrmsr
	popal
	ret

gpHandler:
	// Skip the faulting RDMSR (two bytes); real mode pushes no error code.
	push %bp
	mov %sp, %bp
	addw $2, 2(%bp)
	pop %bp
	incl 0x901C
	iret

timerHandler:
	incl 0x9028
	call eoi
	iret

ipiHandler:
	incl 0x902C
	call eoi
	iret

assertHandler:
	incl 0x9030
	call eoi
	iret

levelHandler:
	incl 0x9034
	call eoi
	iret
guestCodeEnd:

.code64
.popsection
)");

extern "C" char guestCode[], guestCodeEnd[];

namespace {

int failures = 0;

#define CHECK(cond, ...) do { \
	if(!(cond)) { \
		printf("virt-test: FAIL: %s: ", #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		fflush(stdout); \
		failures++; \
	} \
} while(0)

[[noreturn]] void fatal(const char *what, const HelVmexitReason &reason) {
	printf("virt-test: FAIL: %s (exit %d, code 0x%x, address 0x%lx, flags 0x%lx)\n",
			what, static_cast<int32_t>(reason.exitReason), reason.code,
			reason.address, reason.flags);
	fflush(stdout);
	_exit(1);
}

void setRealMode(HelHandle vcpu, uint64_t rip) {
	HelX86VirtualizationRegs regs{};
	HEL_CHECK(helLoadRegisters(vcpu, kHelRegsVirtualization, &regs));

	HelX86SegmentRegister seg{};
	seg.limit = 0xFFFF;
	seg.type = 3;
	seg.s = 1;
	seg.present = 1;
	regs.ds = seg;
	regs.es = seg;
	regs.fs = seg;
	regs.gs = seg;
	regs.ss = seg;
	regs.cs = seg;
	regs.cs.type = 0xB;

	regs.ldt = {};
	regs.ldt.type = 2;
	regs.ldt.present = 1;
	regs.ldt.limit = 0xFFFF;
	regs.tr = {};
	regs.tr.limit = 0xFFFF;
	regs.tr.type = 3;
	regs.tr.present = 1;
	regs.gdt = {0, 0xFFFF};
	regs.idt = {0, 0xFFFF};

	regs.rip = rip;
	regs.rflags = 1 << 1;
	HEL_CHECK(helStoreRegisters(vcpu, kHelRegsVirtualization, &regs));
}

void *watchdog(void *) {
	sleep(60);
	printf("virt-test: FAIL: timeout\n");
	fflush(stdout);
	_exit(1);
}

} // namespace

int main() {
	pthread_t watchdogThread;
	pthread_create(&watchdogThread, nullptr, watchdog, nullptr);

	HelHandle vspace, mem, vcpu;
	HEL_CHECK(helCreateVirtualizedSpace(&vspace));
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), memorySize, 0, nullptr, &mem));

	void *fakePtr;
	HEL_CHECK(helMapMemory(mem, vspace, nullptr, 0, memorySize,
			kHelMapFixed | kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute, &fakePtr));
	assert(fakePtr == nullptr);
	void *ptr;
	HEL_CHECK(helMapMemory(mem, kHelNullHandle, nullptr, 0, memorySize,
			kHelMapProtRead | kHelMapProtWrite, &ptr));
	auto guest = static_cast<uint8_t *>(ptr);
	memcpy(guest + entry, guestCode, guestCodeEnd - guestCode);
	auto result = [&] (uint32_t offset) {
		uint32_t value;
		memcpy(&value, guest + results + offset, sizeof(value));
		return value;
	};

	HEL_CHECK(helCreateVirtualizedCpu(vspace, &vcpu));
	setRealMode(vcpu, entry);

	HelVmexitReason reason{};
	HelVmexitCompletion completion{};

	// A completion without a pending exit is refused.
	CHECK(helRunVirtualizedCpu(vcpu, &completion, &reason) == kHelErrIllegalState,
			"completion without a pending exit");

	// A kick before the run makes it return immediately.
	HEL_CHECK(helKickVirtualizedCpu(vcpu));
	HEL_CHECK(helRunVirtualizedCpu(vcpu, nullptr, &reason));
	CHECK(static_cast<int32_t>(reason.exitReason) == kHelVmexitKicked,
			"exit %d after a kick", static_cast<int32_t>(reason.exitReason));

	bool complete = false;
	bool testedMissingCompletion = false;
	bool sawLevelEoi = false;
	while(true) {
		HEL_CHECK(helRunVirtualizedCpu(vcpu, complete ? &completion : nullptr, &reason));
		complete = false;
		completion = {};
		auto exitReason = static_cast<int32_t>(reason.exitReason);

		if(exitReason == kHelVmexitCpuid) {
			CHECK(reason.address == 0x12345678 && reason.data == 9,
					"CPUID leaf 0x%lx, subleaf 0x%lx", reason.address, reason.data);
			// Running again without completing the CPUID is refused.
			if(!testedMissingCompletion) {
				HelVmexitReason dummy;
				CHECK(helRunVirtualizedCpu(vcpu, nullptr, &dummy) == kHelErrIllegalState,
						"missing completion");
				testedMissingCompletion = true;
			}
			completion.status = kHelVmexitCompletionDone;
			completion.data[0] = 0x11;
			completion.data[1] = 0x22;
			completion.data[2] = 0x33;
			completion.data[3] = 0x44;
			complete = true;
		}else if(exitReason == kHelVmexitIo) {
			complete = true;
			completion.status = kHelVmexitCompletionDone;
			if(reason.address == portIn) {
				CHECK((reason.flags & kHelVmexitIoIn) && reason.code == 4, "IN flags");
				completion.data[0] = 0xCAFEBABE;
			}else if(reason.address == portAssert) {
				CHECK(!(reason.flags & kHelVmexitIoIn) && reason.code == 1, "OUT flags");
				bool level = reason.data == 0x43;
				HEL_CHECK(helAssertVirtualizedIrq(vcpu, reason.data, level));
			}else if(reason.address == portDone) {
				break;
			}else{
				fatal("unexpected port", reason);
			}
		}else if(exitReason == kHelVmexitRdmsr) {
			complete = true;
			if(reason.address == msrCompleted) {
				completion.status = kHelVmexitCompletionDone;
				completion.data[0] = 0x1122334455667788;
			}else{
				CHECK(reason.address == msrFaulted, "RDMSR 0x%lx", reason.address);
				completion.status = kHelVmexitCompletionFault;
			}
		}else if(exitReason == kHelVmexitEoi) {
			CHECK(reason.code == 0x43, "EOI of vector 0x%x", reason.code);
			sawLevelEoi = true;
		}else{
			fatal("unexpected exit", reason);
		}
	}

	HelX86VirtualizationRegs regs;
	HEL_CHECK(helLoadRegisters(vcpu, kHelRegsVirtualization, &regs));
	CHECK(regs.apic_base == 0xFEE00D00, "IA32_APIC_BASE 0x%lx", regs.apic_base);

	CHECK(result(resCpuidEax) == 0x11 && result(resCpuidEbx) == 0x22
			&& result(resCpuidEcx) == 0x33 && result(resCpuidEdx) == 0x44, "CPUID result");
	CHECK(result(resIn) == 0xCAFEBABE, "IN result 0x%x", result(resIn));
	CHECK(result(resMsrLow) == 0x55667788 && result(resMsrHigh) == 0x11223344,
			"RDMSR result 0x%x:%x", result(resMsrHigh), result(resMsrLow));
	CHECK(result(resGpCount) == 2, "#GP count %u", result(resGpCount));
	CHECK(result(resApicId) == 0, "APIC ID %u", result(resApicId));
	CHECK(result(resApicVersion) == 0x50014, "APIC version 0x%x", result(resApicVersion));
	CHECK(result(resTimerCount) >= 2, "timer count %u", result(resTimerCount));
	CHECK(result(resIpiCount) == 2, "IPI count %u", result(resIpiCount));
	CHECK(result(resAssertCount) == 1, "edge interrupt count %u", result(resAssertCount));
	CHECK(result(resLevelCount) == 1, "level interrupt count %u", result(resLevelCount));
	CHECK(sawLevelEoi, "EOI exit");

	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, vcpu));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, vspace));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, mem));

	if(failures) {
		printf("virt-test: %d check(s) failed\n", failures);
		return 1;
	}
	printf("virt-test: PASS\n");
	return 0;
}
