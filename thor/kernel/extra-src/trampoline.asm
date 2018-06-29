
.set .L_kRflagsIf, 0x200

.set .L_kEferLme, 0x100
.set .L_kEferNx, 0x800

.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

.set statusBlock, 0xFD8
.set statusTargetStage, 0xFD8
.set statusInitiatorStage, 0xFDC
.set statusPml4, 0xFE0
.set statusStack, 0xFE8
.set statusMain, 0xFF0
.set statusCpuContext, 0xFF8

.code16
.global trampoline
trampoline:
	cli

	# We assume that the APIC SIPI loads IP with 0.
	mov %cs, %bx
	mov %bx, %ds

	# Load our base address into EBX. We need it once we enter a linear address space.
	shl $4, %ebx

	# Inform the BSP that we're awake.
	movl $1, statusTargetStage
	
	# Wait until BSP code allows us to proceed.
.L_spin:
	cmp $1, statusInitiatorStage
	jne .L_spin

	# Now we can initialize the processor and jump into kernel code.

	# Enter protected mode.
	mov %ebx, %eax # Cannot use LEA because of real mode segment limits.
	add $.L_gdt_start, %eax
	mov %eax, .L_gdtr + 2
	lgdt .L_gdtr

	mov %cr0, %eax
	or $1, %eax
	mov %eax, %cr0

	mov %ebx, %eax # Cannot use LEA because of real mode segment limits.
	add $.L_protected, %eax
	mov %eax, .L_pmjump
	ljmpl *.L_pmjump

.code32
.L_protected:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	xor %ax, %ax
	mov %ax, %fs
	mov %ax, %gs

	# Setup the PAT. Keep this in sync with the eir code.
	mov $0x00000406, %eax
	mov $0x00000100, %edx
	mov $0x277, %ecx
	wrmsr

	# Enable PAE paging.
	mov %cr4, %eax
	or $0x20, %eax
	mov %eax, %cr4
	
	# Enable long mode (not active until we enable paging).
	mov $0xC0000080, %ecx
	rdmsr
	or $(.L_kEferLme | .L_kEferNx), %eax
	wrmsr
	
	# Setup the PML4.
	mov statusPml4(%ebx), %eax
	mov %eax, %cr3
	
	# Enable paging + WP flag.
	mov %cr0, %eax
	or $0x80010000, %eax
	mov %eax, %cr0
	
	lea .L_longmode(%ebx), %eax
	mov %eax, .L_lmjump(%ebx)
	ljmpl *.L_lmjump(%ebx)

.code64
.L_longmode:
	xor %ax, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov %ax, %fs
	mov %ax, %gs
	
	# Enable SSE support.
	mov %cr0, %rax
	and $0xFFFFFFFFFFFFFFFB, %rax # Disable EM.
	or $2, %rax # enable MP
	mov %rax, %cr0

	mov %cr4, %rax
	or $0x200, %rax # Enable OSFXSR.
	or $0x400, %rax # Enable OSXMMEXCPT.
	mov %rax, %cr4

	mov statusStack(%rbx), %rsp
	lea statusBlock(%rbx), %rdi
	call *statusMain(%rbx)
	ud2

.align 16
.L_gdt_start:
	.int 0
	.int 0
	.int 0x0000FFFF
	.int 0x00CF9800
	.int 0x0000FFFF
	.int 0x00CF9200
	.int 0
	.int 0x00A09800
.L_gdt_end:

# LGDT expects a linear address. Because we do not know our base address
# we fill in the real address at run time.
.align 4
.L_gdtr:
	.short .L_gdt_end - .L_gdt_start
	.long 0

# We don't have a stack when entering protected mode and our base address is not
# fixed so we use an indirect far jump to enter protected mode.
.align 4
.L_pmjump:
	.long 0
	.short 0x08

.align 4
.L_lmjump:
	.long 0
	.short 0x18

