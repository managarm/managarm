.section .text.init
.global eirEntry
eirEntry:
	.extern eirStackTop
	ldr x1, =eirStackTop
	mov sp, x1

	// Load vector table
	ldr x1, =eirExcVectors
	msr vbar_el1, x1

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
	.extern eirVirtMain
	bl eirVirtMain

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
