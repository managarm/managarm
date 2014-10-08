
.section .header
.global pkrt_header
pkrt_header:
	.int 0x1BADB002
	.int 0
	.int -0x1BADB002

.section .data
pkrt_gdtr:
	.short 0
	.int 0
pkrt_idtr:
	.short 0
	.int 0

.section .text
.global mb_entry
mb_entry:
	mov $0x200000, %esp
	call prekernel_main

halt_kernel:
	hlt
	jmp halt_kernel

.global pkrt_kernel
pkrt_kernel:
	// enable PAE paging
	mov %cr4, %eax
	or $0x20, %eax
	mov %eax, %cr4
	
	// enable long mode (not active until we enable paging) */
	mov $0xC0000080, %ecx
	rdmsr
	or $0x100, %eax
	wrmsr
	
	// setup the pml4
	mov 4(%esp), %eax
	mov %eax, %cr3
	
	// enable paging
	mov %cr0, %eax
	or $0x80000000, %eax
	mov %eax, %cr0

	// load a 64 bit code segment
	ljmp $0x18, $pkrt_kernel_64bits
pkrt_kernel_64bits:
.code64
	// load the kernel entry address and jump
	mov 8(%rsp), %rax
	jmp *%rax
	hlt /* should never be reached! */
.code32

.global pkrt_lgdt
pkrt_lgdt:
	mov 4(%esp), %eax
	mov 8(%esp), %ecx
	/* construct gdtr and load it */
	mov %cx, (pkrt_gdtr)
	mov %eax, (pkrt_gdtr + 2)
	lgdt (pkrt_gdtr)
	/* reload code segment */
	ljmp $0x8, $pkrt_lgdt_reload
pkrt_lgdt_reload:
	/* reload data segments */
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov $0, %ax
	mov %ax, %fs
	mov %ax, %gs
	ret

.global pkrt_igdt
pkrt_igdt:
	mov 4(%esp), %eax
	mov 8(%esp), %ecx
	/* construct idtr and load it */
	mov %cx, (pkrt_idtr)
	mov %eax, (pkrt_idtr + 2)
	lidt (pkrt_idtr)
	ret

.global pkrt_trap_stub
pkrt_trap_stub:
	hlt

.section .image
	.balign 0x1000
.global pkrt_image
pkrt_image:
	.incbin "../thor/bin/kernel.elf"

