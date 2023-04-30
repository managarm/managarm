// vim: ft=arm64asm

.set statusSelf, 0xFC8
.set statusTTBR0, 0xFD8
.set statusTTBR1, 0xFE0
.set statusStack, 0xFE8
.set statusMain, 0xFF0
.set statusCpuContext, 0xFF8

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

.L_inEL2:
	// Drop to EL1

	// Enable access to counters
	mrs x0, cnthctl_el2
	orr x0, x0, #3
	msr cnthctl_el2, x0
	msr cntvoff_el2, xzr

	// Enable AArch64
	mov x0, xzr
	orr x0, x0, #(1 << 31)
	orr x0, x0, #(1 << 1)
	msr hcr_el2, x0

	// Don't trap FP/SIMD to EL2
	mov x0, #0x33FF
	msr cptr_el2, x0
	msr hstr_el2, xzr

	// Enter EL1
	mov x0, #0x3C5
	msr spsr_el2, x0
	adr x0, .L_inEL1
	msr elr_el2, x0
	eret

.L_inEL1:
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

	add x0, x1, statusTTBR0
	ldp x0, x2, [x0]
	msr ttbr0_el1, x0
	msr ttbr1_el1, x2

	ldr x0, =0x4404000CFF
	msr mair_el1, x0

	ldr x0, =0x5a5102510
	msr tcr_el1, x0

	add x0, x1, statusMain
	ldr x0, [x0]
	msr elr_el1, x0

	mov x0, #0x3C5
	msr spsr_el1, x0

	add x0, x1, statusStack
	ldr x0, [x0]
	mov sp, x0

	add x0, x1, statusSelf
	ldr x0, [x0]

	mov x30, xzr
	mov x29, xzr

	mrs x2, sctlr_el1
	orr x2, x2, #1
	msr sctlr_el1, x2

	eret

	.section .note.GNU-stack,"",%progbits
