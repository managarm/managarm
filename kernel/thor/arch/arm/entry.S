.data
.align 3
.global thorBootInfoPtr
thorBootInfoPtr:
	.quad 0

.text
.global thorRtEntry
thorRtEntry:
	ldr x1, =thorBootInfoPtr
	str x0, [x1]

	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	bl thorInitialize
	bl thorRunConstructors
	bl thorMain
