.text
.global thorRtEntry
.p2align 2
thorRtEntry:
	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	jal thorInitialize
	jal thorRunConstructors
	j thorMain

.text
.p2align 2
.global thorSmpTrampolineStart
.global thorSmpTrampolineEnd
thorSmpTrampolineStart:
	# Get the address of the trampoline page.
	auipc t0, 0 # t0 = trampoline page + 8 (since code starts at offset 8)

	# Ensure that the TLB is clear.
	sfence.vma

	# Enable paging.
	ld t1, -8(t0)
	csrw satp, t1

	# Jump to the higher half.
	ld sp, 0(a1) # sp.
	ld t1, 8(a1) # entry.
	mv a0, a1
	jr t1
thorSmpTrampolineEnd:
