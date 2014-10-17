
.global zisaRtEntry
zisaRtEntry:
	mov $1, %rdi
	mov $0x1000, %rsi
	int $0x80
	
	mov $2, %rdi
	mov %rax, %rsi
	mov $0x2000000, %rdx
	mov $0x1000, %rcx
	int $0x80
	
	mov $0x2001000, %rsp
	push $1

zisaIdle:
	jmp zisaIdle

