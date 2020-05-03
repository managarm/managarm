.set kEferNx, 0x800

.section .bss
.align 16
.global eirRtStackTop
eirRtStackBottom:
	.skip 0x100000 # reserve 1 MiB for the stack
eirRtStackTop:

.section .text
.global eirRtEnterKernel
eirRtEnterKernel:
	mov %rdi, %cr3
	mov %rdx, %rsp

	mov $0xC0000080, %rcx
	rdmsr
	or $(kEferNx), %eax
	wrmsr

	mov %cr0, %rax
	or $0x00010000, %rax # Enable WP
	mov %rax, %cr0

	xor %ebp, %ebp
	jmp *%rsi
