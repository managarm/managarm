#include <thor-internal/arch/asm.h>

.set .L_uarRead, 1
.set .L_uarWrite, 2

.global doCopyFromUser

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarRead

.text
doCopyFromUser:
	mov $1b, %rax
	mov %gs:THOR_GS_EXECUTOR, %r8
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Setup the UAR.
	mov %rdx, %rcx

2:
	# DF = 0 due to the calling convention.
	rep movsb

3:
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	ret

4:
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	mov $1, %rax
	ret

.global doCopyToUser

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarWrite

.text
doCopyToUser:
	mov $1b, %rax
	mov %gs:THOR_GS_EXECUTOR, %r8
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Setup the UAR.
	mov %rdx, %rcx

2:
	# DF = 0 due to the calling convention.
	rep movsb

3:
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	ret

4:
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	mov $1, %rax
	ret

.global doAtomicUserLoad

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarRead | .L_uarWrite

.text
doAtomicUserLoad:
	mov $1b, %rax
	mov %gs:THOR_GS_EXECUTOR, %r8
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Setup the UAR.

2:
	mov (%rsi), %r11

3:
	mov %r11, (%rdi)
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	ret

4:
	xor %rax, %rax
	mov %rax, THOR_EXECUTOR_UAR(%r8) # Clean up the UAR.
	mov $1, %rax
	ret

	.section .note.GNU-stack,"",%progbits
