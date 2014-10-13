
.global zisaRtEntry
zisaRtEntry:
	int $0x80
	jmp zisaRtEntry

