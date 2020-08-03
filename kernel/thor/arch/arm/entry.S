.data
.align 3
.global thorBootInfoPtr
thorBootInfoPtr:
	.quad 0

.text
.global thorRtEntry
thorRtEntry:
	wfi
	b thorRtEntry
