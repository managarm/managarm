.global eirExcVectors
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
	// Disable MMU
	mrs x1, sctlr_el1
	and x1, x1, #0xFFFFFFFFFFFFFFFE
	msr sctlr_el1, x1

	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
	mov sp, x1
	mrs x1, esr_el1
	mrs x2, elr_el1
	mrs x3, spsr_el1
	mrs x4, far_el1
	b eirExceptionHandler

	.section .note.GNU-stack,"",%progbits
