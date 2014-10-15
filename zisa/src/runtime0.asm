
.global zisaRtEntry
zisaRtEntry:
	mov $1, %rdi
	mov $0x1000, %rsi
	int $0x80
zisaIdle:
	jmp zisaIdle

