.data
.global thorBootInfoPtr
thorBootInfoPtr:
	.quad 0

.text
.global thorRtEntry
thorRtEntry:
	la a0, thorBootInfoPtr

	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	jal thorInitialize
	jal thorRunConstructors
	j thorMain