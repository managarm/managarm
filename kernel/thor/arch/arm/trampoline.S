// vim: ft=arm64asm

#include <thor-internal/arch/asm.h>

.global apEntryPoint
apEntryPoint:
	bl .L_entry
.L_entry:
	// Determine our base address
	sub x1, x30, #4

	mrs x2, currentel
	cmp x2, #8
	// TODO: handle EL3
	b.eq .L_inEL2
	cmp x2, #4
	b.eq .L_inEL1

	// If we're in EL0 we're in trouble anyway
.L_halt:
	b .L_halt

.L_inEL1:
	// There is no way back to EL2 from here, hence we cannot join a VHE kernel.
	add x0, x1, THOR_AP_STATUS_USE_VHE
	ldr w0, [x0]
	cbnz w0, .L_halt

	// x3 holds the PSTATE that we run the kernel with.
	mov x3, #0x3C5
	b .L_kernelEl

.L_inEL2:
	msr cntvoff_el2, xzr
	msr hstr_el2, xzr

	// Stay in EL2 if the kernel runs with VHE.
	add x0, x1, THOR_AP_STATUS_USE_VHE
	ldr w0, [x0]
	cbnz w0, .L_enterE2h

	// Drop to EL1

	// Don't trap EL1 access to the counters
	mrs x0, cnthctl_el2
	orr x0, x0, #3 // EL1PCTEN and EL1PCEN
	msr cnthctl_el2, x0

	// Enable AArch64
	mov x0, xzr
	orr x0, x0, #(1 << 31)
	orr x0, x0, #(1 << 1)
	msr hcr_el2, x0

	// Don't trap FP/SIMD to EL2
	mov x0, #0x33FF
	msr cptr_el2, x0

	// Enter EL1
	mov x0, #0x3C5
	msr spsr_el2, x0
	adr x0, .L_kernelEl
	msr elr_el2, x0
	mov x3, #0x3C5
	eret

.L_enterE2h:
	mov x0, xzr
	orr x0, x0, #(1 << 34) // E2H
	orr x0, x0, #(1 << 31) // RW
	orr x0, x0, #(1 << 27) // TGE
	orr x0, x0, #(1 << 1)  // SWIO
	msr hcr_el2, x0
	isb
	// Flush the TLB since the E2H bit may be cached.
	tlbi alle2
	tlbi alle1
	dsb ish
	isb

	// Don't trap EL0 access to the counters.
	// CNTHCTL_EL2 changes its layout once E2H is set. We do not preserve the
	// other bits since PSCI hands us the register with an unknown reset value.
	mov x0, #3 // EL0PCTEN and EL0VCTEN
	msr cnthctl_el2, x0

	// Don't trap FP/SIMD. CPTR_EL2 has the CPACR_EL1 layout once E2H is set.
	mov x0, #(3 << 20)
	msr cptr_el2, x0

	mov x3, #0x3C9

.L_kernelEl:
	mov x0, xzr
	orr x0, x0, #(1 << 29)
	orr x0, x0, #(1 << 28)
	orr x0, x0, #(1 << 23)
	orr x0, x0, #(1 << 22)
	orr x0, x0, #(1 << 20)
	orr x0, x0, #(1 << 11)
	orr x0, x0, #(1 << 12)
	orr x0, x0, #(1 << 2)
	msr sctlr_el1, x0
	isb

	tlbi vmalle1
	dsb ish
	isb

	add x0, x1, THOR_AP_STATUS_TTBR0
	ldp x0, x2, [x0]
	msr ttbr0_el1, x0
	msr ttbr1_el1, x2

	ldr x0, =0x4404000CFF
	msr mair_el1, x0

	ldr x0, =0x5a5102510
	msr tcr_el1, x0

	add x0, x1, THOR_AP_STATUS_MAIN
	ldr x0, [x0]
	msr elr_el1, x0

	msr spsr_el1, x3

	add x0, x1, THOR_AP_STATUS_STACK
	ldr x0, [x0]
	mov sp, x0

	add x0, x1, THOR_AP_STATUS_SELF
	ldr x0, [x0]

	mov x30, xzr
	mov x29, xzr

	mrs x2, sctlr_el1
	orr x2, x2, #1
	msr sctlr_el1, x2

	eret

	.section .note.GNU-stack,"",%progbits
