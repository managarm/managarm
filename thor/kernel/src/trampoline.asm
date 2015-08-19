
.code16
.section .trampoline
	movw $0x1100, %ax
	movw %ax, %ds
	movl $1, %ds:0
	cli
	hlt

