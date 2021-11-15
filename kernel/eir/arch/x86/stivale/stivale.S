.section .stivale2hdr
.extern eirStackTop
eirStivaleHeader:
	.quad 0
	.quad eirStackTop
	.quad (1 << 4) // Bit 4: disable deprecated feature, always set
	.quad eirStivaleFbTag

.section .rodata
eirStivaleFbTag:
	.quad 0x3ecc1bc43d0f7971
	.quad 0
	.word 0
	.word 0
	.word 0
