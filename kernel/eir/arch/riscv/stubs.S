.global eirEnterKernel
eirEnterKernel:
	csrw satp, a0
	mv sp, a2
	jr a1

.section .note.GNU-stack,"",%progbits
