
.global thorRtEntry
thorRtEntry:
	# enable SSE support
	mov %cr0, %rax
	and $0xFFFFFFFFFFFFFFFB, %rax # disable EM
	or $2, %rax # enable MP
	mov %rax, %cr0

	mov %cr4, %rax
	or $0x200, %rax # enable OSFXSR
	or $0x400, %rax # enable OSXMMEXCPT
	mov %rax, %cr4

	call thorInitialize
	call thorRunConstructors
	call thorMain
	ud2

.text
.global enableIntsAndHaltForever
enableIntsAndHaltForever:
	pushq $0x58
	pushq $enter_context
	lretq
enter_context:
	sti
halt_loop:
	hlt
	jmp halt_loop

