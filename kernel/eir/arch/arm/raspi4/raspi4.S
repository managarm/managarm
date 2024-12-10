.section .text.init
.global eirEntry
eirEntry:
	b .L1
	nop
	.quad 0x80000 // Image offset
	.quad 0 // Image size (0 = unbounded)
	.quad 2 // 4K pages
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0x644d5241 // Magic
	.quad 0 // Reserved

.L1:
	mov x7, x0

	.extern eirStackTop
	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
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
	adrp x0, eirExcVectors
	add x0, x0, :lo12:eirExcVectors
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
	adrp x5, eirBssStart
	adrp x6, eirBssEnd
	add x5, x5, :lo12:eirBssStart
	add x6, x6, :lo12:eirBssEnd
.loop:
	cmp x5, x6
	b.eq .enter
	str xzr, [x5]
	add x5, x5, #8
	b .loop

.enter:
	.extern eirRaspi4Main

	// x7 -> pointer to device tree binary
	adrp x1, eirDtbPtr
	add x1, x1, :lo12:eirDtbPtr

	// the device tree pointer is 32-bit and the upper bits are undefined
	uxtw x7, w7
	str x7, [x1]

	bl eirRaspi4Main

.halt:
	wfe
	b .halt

	.section .note.GNU-stack,"",%progbits
