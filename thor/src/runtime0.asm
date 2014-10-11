
.global thorRtEntry
thorRtEntry:
	.extern thorMain
	call thorMain
	hlt

.global thorRtHalt
thorRtHalt:
	hlt
	jmp thorRtHalt

