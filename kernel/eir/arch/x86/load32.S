.set kEferLme, 0x100
.set kEferNx, 0x800

.section .bss
	.align 16
eirStackBottom:
	.skip 0x100000 # reserve 1 MiB for the stack
	.global eirStackTop
eirStackTop:

.section .trampoline
.global eirEnterKernel
eirEnterKernel:
	# enable PAE paging
	mov %cr4, %eax
	or $0x20, %eax
	mov %eax, %cr4
	
	# enable long mode (not active until we enable paging)
	mov $0xC0000080, %ecx
	rdmsr
	or $(kEferLme | kEferNx), %eax
	wrmsr
	
	# setup the pml4
	mov 4(%esp), %eax
	mov %eax, %cr3

	# edx:eax <- thor entry
	mov 8(%esp), %eax
	mov 12(%esp), %edx

	# ecx:ebx <- thor stack pointer
	mov 16(%esp), %ebx
	mov 20(%esp), %ecx
	
	# enable paging + WP bit
	# note: we cannot access the stack after this jump
	mov %cr0, %esi
	or $0x80010000, %esi
	mov %esi, %cr0

	# load a 64 bit code segment
	ljmp $0x18, $.Lkernel_64bits

.Lkernel_64bits:
.code64
	mov $0, %si
	mov %si, %ds
	mov %si, %es
	mov %si, %ss
	
	# rax <- thor entry
	shl $32, %rdx
	or %rax, %rdx

	# rsp <- thor stack
	shl $32, %rcx
	or %rbx, %rcx
	mov %rcx, %rsp

	xor %rbp, %rbp
	jmp *%rdx

	.section .note.GNU-stack,"",%progbits
