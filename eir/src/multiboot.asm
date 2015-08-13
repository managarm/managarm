
.set kEferLme, 0x100
.set kEferNx, 0x800

.section .header
	.int 0x1BADB002
	.int 0
	.int -0x1BADB002

.section .data
eirRtGdtr:
	.short 0
	.int 0

.section .bss
	.align 16
eirRtStackBottom:
	.skip 0x100000 # reserve 1 MiB for the stack
eirRtStackTop:

.section .text
.global eirRtEntry
eirRtEntry:
	mov $eirRtStackTop, %esp
	push %ebx
	call eirMain

halt_kernel:
	hlt
	jmp halt_kernel

.global eirRtEnterKernel
eirRtEnterKernel:
	mov 24(%esp), %edi

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
	
	# enable paging
	mov %cr0, %eax
	or $0x80000000, %eax
	mov %eax, %cr0

	# load a 64 bit code segment
	ljmp $0x18, $pkrt_kernel_64bits

pkrt_kernel_64bits:
.code64
	mov $0, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	
	# load the kernel entry address and jump
	mov 8(%rsp), %rax
	mov 16(%rsp), %rsp
	jmp *%rax
.code32

.global eirRtLoadGdt
eirRtLoadGdt:
	mov 4(%esp), %eax
	mov 8(%esp), %ecx
	
	# construct gdtr and load it
	mov %cx, (eirRtGdtr)
	mov %eax, (eirRtGdtr + 2)
	lgdt (eirRtGdtr)

	# reload code segment
	ljmp $0x8, $gdt_reload

gdt_reload:
	# reload data segments
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov $0, %ax
	mov %ax, %fs
	mov %ax, %gs
	ret

