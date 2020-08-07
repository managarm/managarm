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

	// Load vector table
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

.align 11
eirExcVectors:
	// EL1t
	// Synchronous
.align 7
	mov x0, #0
	b eirCommonHandler

	// IRQ
.align 7
	mov x0, #1
	b eirCommonHandler

	// FIQ
.align 7
	mov x0, #2
	b eirCommonHandler

	// SError
.align 7
	mov x0, #3
	b eirCommonHandler


	// EL1h
	// Synchronous
.align 7
	mov x0, #0
	b eirCommonHandler

	// IRQ
.align 7
	mov x0, #1
	b eirCommonHandler

	// FIQ
.align 7
	mov x0, #2
	b eirCommonHandler

	// SError
.align 7
	mov x0, #3
	b eirCommonHandler

eirCommonHandler:
	ldr x1, =eirStackTop
	mov sp, x1
	mrs x1, esr_el1
	mrs x2, elr_el1
	mrs x3, spsr_el1
	mrs x4, far_el1
	b eirExceptionHandler
