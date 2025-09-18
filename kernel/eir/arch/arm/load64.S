.section .bss
.align 16
.global eirStackTop
eirStackBottom:
	.skip 0x100000 // reserve 1 MiB for the stack
eirStackTop:

// Flushes the data cache to PoC in a certain range.
// ABI:
// x0: start address (inclusive)
// x1: end address (exclusive)
// x2: data cache line size
.macro FLUSH_DC
1:
	dc cvac, x0
	add x0, x0, x2
	cmp x0, x1
	b.lo 1b
	// Ensure that the cache flush is finished.
	dsb ish
	isb
.endm

.section .text
.global eirFlushDisableMmuEl1
eirFlushDisableMmuEl1:
	// x0, x1, x2: see FLUSH_DC
	// x3: new sctlr_el1 (MMU bit is ignored).
	FLUSH_DC
	bic x3, x3, #1
	msr sctlr_el1, x3
	isb
	ret

.section .text
.global eirEnterKernel
eirEnterKernel:
	// x0 -> Thor entry pointer
	// x1 -> Thor stack pointer
	mov sp, x1
	br x0

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
