
.global pltRelocateStub
pltRelocateStub:
	push %rax
	push %rcx
	push %rdx
	push %rsi
	push %rdi
	push %r10

	mov 48(%rsp), %rdi
	mov 56(%rsp), %rsi
	call lazyRelocate
	mov %rax, %r11

	pop %r10
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rcx
	pop %rax
	add $16, %rsp

	jmp *%r11

