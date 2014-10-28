
.global zisaRtEntry
zisaRtEntry:
	# create a memory descriptor
	mov $2, %rdi
	mov $0x1000, %rsi
	int $0x80
	
	# map stack memory into this address space
	mov $5, %rdi
	mov %rax, %rsi
	mov $0x2000000, %rdx
	mov $0x1000, %rcx
	int $0x80
	
	mov $0x2001000, %rsp

	.extern main
	call main

zisaIdle:
	jmp zisaIdle

.global syscall2
syscall2:
	int $0x80
	ret

