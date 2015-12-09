
.global _start
_start:
	call interpreterMain

	jmp *%rax

.global pltRelocateStub
pltRelocateStub:
	# we need to save / restore all registers than can hold function arguments
	# we do not need to save callee-saved registers as they will not be trashed by lazyRelocate
	# TODO: save floating point argument registers

	push %rsi
	push %rdi
	mov 16(%rsp), %rdi
	mov 24(%rsp), %rsi

	push %rax
	push %rcx
	push %rdx
	push %r8
	push %r9
	push %r10

	call lazyRelocate
	mov %rax, %r11

	pop %r10
	pop %r9
	pop %r8
	pop %rdx
	pop %rcx
	pop %rax
	
	pop %rdi
	pop %rsi
	add $16, %rsp
	jmp *%r11

