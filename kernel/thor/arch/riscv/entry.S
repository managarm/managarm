.text
.global thorRtEntry
thorRtEntry:
	.extern thorInitialize
	.extern thorRunConstructors
	.extern thorMain
	jal thorInitialize
	jal thorRunConstructors
	j thorMain
