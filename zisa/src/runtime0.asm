
.global zisaRtEntry
zisaRtEntry:
	mov $1, %rdi
	mov $0x1000, %rsi
	int $0x80
	
	mov $3, %rdi
	mov %rax, %rsi
	mov $0x2000000, %rdx
	mov $0x1000, %rcx
	int $0x80
	
	mov $0x2001000, %rsp

	mov $2, %rdi
	int $0x80

	mov $4, %rdi
	mov %rax, %rsi
	int $0x80

zisaIdle:
	jmp zisaIdle

