.section .text.init
.global eirEntry
eirEntry:
	mov x7, x0

	.extern eirStackTop
	ldr x1, =eirStackTop
	mov sp, x1

	// Get current execution level
	mrs x0, currentel
	and x0, x0, #12
	cmp x0, #8
	// Halt if not in EL2
	b.ne .halt

	// Drop into EL1

	msr sp_el1, x1

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

	// Load vector table
	.extern eirExcVectors
	ldr x0, =eirExcVectors
	msr vbar_el1, x0

	// "Return" into EL1
	mov x0, #0x3c5
	msr spsr_el2, x0
	adr x0, .in_el1
	msr elr_el2, x0
	eret

.in_el1:
	mov sp, x1

	// Zero out BSS
	.extern eirBssStart
	.extern eirBssEnd
	ldr x5, =eirBssStart
	ldr x6, =eirBssEnd
.loop:
	cmp x5, x6
	b.eq .enter
	str xzr, [x5]
	add x5, x5, #8
	b .loop

.enter:
	.extern eirRaspi4Main
	mov x0, x7
	// x0 -> pointer to device tree binary
	bl eirRaspi4Main

.halt:
	wfe
	b .halt
