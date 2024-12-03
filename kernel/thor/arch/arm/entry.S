.text
.global thorRtEntry
thorRtEntry:
	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	bl thorInitialize
	bl thorRunConstructors
	bl thorMain

	.section .note.GNU-stack,"",%progbits
