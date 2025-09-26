.section .text.init
.global eirEntry
eirEntry:
	b .L1
	nop
	.quad 0 // Image offset
	// Image size (0 = unbounded)
	// Emit as two .long to avoid issues with relocations.
	.long eirImageSize
	.long 0
	.quad 2 // 4K pages
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0x644d5241 // Magic
	.quad 0 // Reserved

.L1:
	.extern eirStackTop
	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
	mov sp, x1

	mov x2, xzr
	orr x2, x2, #(1 << 29)
	orr x2, x2, #(1 << 28)
	orr x2, x2, #(1 << 23)
	orr x2, x2, #(1 << 22)
	orr x2, x2, #(1 << 20)
	orr x2, x2, #(1 << 11)
	orr x2, x2, #(1 << 12)
	orr x2, x2, #(1 << 2)
	msr sctlr_el1, x2

	// Load vector table
	.extern eirExcVectors
	adrp x1, eirExcVectors
	add x1, x1, :lo12:eirExcVectors
	msr vbar_el1, x1

.enter:
	adrp x1, eirDtbPtr
	add x1, x1, :lo12:eirDtbPtr
	str x0, [x1]

	.extern eirRelocate
	.extern eirRunConstructors
	.extern eirMain
	bl eirRelocate
	bl eirRunConstructors
	bl eirMain

.halt:
	wfe
	b .halt

	.section .note.GNU-stack,"",%progbits
