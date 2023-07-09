.global _start
_start:
	la sp, eirStackTop
	j _start

	la t0, eirDtbPtr
	sd a1, 0(t0)

	jal eirRunConstructors
	j eirMain

.section .bss
eirStackBase:
	.space 0x10000
eirStackTop:

	.section .note.GNU-stack,"",%progbits
