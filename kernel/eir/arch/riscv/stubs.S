.global eirEnterKernel
eirEnterKernel:
	csrw satp, a0
	mv sp, a2
	li a0, 0xFFFFFE8000010000
	jr a1

.section .note.GNU-stack,"",%progbits
