.section .bss
.align 16
.global eirStackTop
eirStackBottom:
	.skip 0x100000 // reserve 1 MiB for the stack
eirStackTop:

.section .text
.global eirEnterKernel
eirEnterKernel:
	// x0 -> TTBR0 ptr
	// x1 -> TTBR1 ptr
	// x2 -> thor entry point
	// x3 -> thor stack pointer
	// x4 -> thor info ptr

	// Set page table pointers
	msr TTBR0_EL1, x0
	msr TTBR1_EL1, x1
	dsb ish
	isb

	// Enable the MMU
	mrs x0, SCTLR_EL1
	orr x0, x0, #1
	msr SCTLR_EL1, x0
	isb

	mov sp, x3
	mov x0, x4
	br x2

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
