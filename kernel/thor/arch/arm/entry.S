.global thorBootInfoPtr
thorBootInfoPtr:
	.quad 0

.global thorRtEntry
thorRtEntry:
	wfi
	b thorRtEntry
