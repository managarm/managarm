# Kernel GS segment fields.
.set .L_gsIseqPtr, 0x18

.global iseqStore64

.data
1: # IseqRegions struct.
	.quad 2f
	.quad 3f
	.quad 4f

.text
# rdi: Destination pointer
# rsi: Destination word
iseqStore64:
	mov %gs:.L_gsIseqPtr, %r8 # Obtain the IseqContext.
	mov $1b, %rax
	mov %rax, (%r8)

2: # startIp.
	# Exit if transaction was interrupted.
	testb $2, 8(%r8)
	jnz 4f

	# Do the store.
	mov %rsi, (%rdi)

3: # commitIp.
	movq $0, (%r8)
	mov $1, %rax
	ret

4: # interruptIp.
	movq $0, (%r8)
	xor %rax, %rax
	ret

.global iseqCopyWeak

.data
1: # IseqRegions struct.
	.quad 2f
	.quad 3f
	.quad 4f

.text
# rdi: Destination pointer
# rsi: Source pointer
# rdx: Size
iseqCopyWeak:
	mov %gs:.L_gsIseqPtr, %r8 # Obtain the IseqContext.
	mov $1b, %rax
	mov %rax, (%r8)

2: # startIp.
	# Exit if transaction was interrupted.
	testb $2, 8(%r8)
	jnz 4f

	# Do the copy.
	# DF is clear due to the calling convention.
	mov %rdx, %rcx
	rep movsb

3: # commitIp.
	movq $0, (%r8)
	mov $1, %rax
	ret

4: # interruptIp.
	movq $0, (%r8)
	xor %rax, %rax
	ret

	.section .note.GNU-stack,"",%progbits
