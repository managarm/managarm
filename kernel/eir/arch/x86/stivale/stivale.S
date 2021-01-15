.section .stivale2hdr
.extern eirStackTop
eirStivaleHeader:
	.quad 0
	.quad eirStackTop
	.quad 0
	.quad eirStivaleFbTag

.section .rodata
eirStivaleFbTag:
	.quad 0x3ecc1bc43d0f7971
	.quad 0
	.word 0
	.word 0
	.word 0

.section .text
.global eirEntry
eirEntry:
	.extern eirStivaleMain
	xor %rbp, %rbp
	jmp eirStivaleMain
.L1:
	hlt
	jmp .L1
