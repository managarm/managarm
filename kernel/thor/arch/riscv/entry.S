.data
.global thorBootInfoPtr
thorBootInfoPtr:
	.quad 0

.text
.global thorRtEntry
thorRtEntry:
	la t0, thorBootInfoPtr
	sd a0, (t0)

	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	jal thorInitialize
	jal thorRunConstructors
	j thorMain
