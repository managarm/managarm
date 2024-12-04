.global eirEnterKernel
eirEnterKernel:
	# Synchronize icache after kernel ELF was loaded.
	fence.i
	# Invaldiate mappings in case mappings by the loader collide with our mappings.
	sfence.vma

	csrw satp, a0
	mv sp, a2
	jr a1

.section .note.GNU-stack,"",%progbits
