
# kernel gs segment fields
.set .L_gsActiveExecutor, 0x08

# executor struct fields
.set .L_executorImagePtr, 0x00
.set .L_executorKernelStack, 0x08

# fields of the state structs
.set .L_imageRax, 0x0
.set .L_imageRbx, 0x8
.set .L_imageRcx, 0x10
.set .L_imageRdx, 0x18
.set .L_imageRsi, 0x20
.set .L_imageRdi, 0x28
.set .L_imageRbp, 0x30

.set .L_imageR8, 0x38
.set .L_imageR9, 0x40
.set .L_imageR10, 0x48
.set .L_imageR11, 0x50
.set .L_imageR12, 0x58
.set .L_imageR13, 0x60
.set .L_imageR14, 0x68
.set .L_imageR15, 0x70

.set .L_imageRip, 0x78
.set .L_imageCs, 0x80
.set .L_imageRflags, 0x88
.set .L_imageRsp, 0x90
.set .L_imageSs, 0x98
.set .L_imageClientFs, 0xA0
.set .L_imageClientGs, 0xA8

.set .L_imageFxSave, 0xB0

.set .L_msrIndexFsBase, 0xC0000100
.set .L_msrIndexGsBase, 0xC0000101
.set .L_msrIndexKernelGsBase, 0xC0000102

# ---------------------------------------------------------
# Fault stubs
# ---------------------------------------------------------

.set .L_typeFaultNoCode, 1
.set .L_typeFaultWithCode, 2
.set .L_typeCall, 3

.macro MAKE_FAULT_STUB type, name, number=0
.section .text.stubs
.global \name
\name:
	# if there is no error code we push a fake one to keep the image struct intact
.if \type != .L_typeFaultWithCode
	push $0
.endif
	# we're pushing 15 registers here.
	# adjust the rsp offsets below if you change those pushs.
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r11
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rcx
	push %rbx
	push %rax

	mov %rsp, %rdi
	mov $\number, %rsi
	call onPlatformFault

	pop %rax
	pop %rbx
	pop %rcx
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r11
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	add $8, %rsp
	iretq
.endm

MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDivideByZero, 0
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDebug, 1
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubBreakpoint, 3
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubOpcode, 6
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubNoFpu, 7
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubDouble, 8
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubProtection, 13
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubPage, 14

# TODO: handle this as an IRQ
#MAKE_FAULT_STUB .L_typeCall, thorRtIsrPreempted, onPreemption

# ---------------------------------------------------------
# IRQ stubs
# ---------------------------------------------------------

.macro MAKE_IRQ_STUB name, number
.section .text.stubs
.global \name
\name:
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r11
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rcx
	push %rbx
	push %rax
	
	mov %rsp, %rdi
	mov $\number, %rsi
	call onPlatformIrq

	pop %rax
	pop %rbx
	pop %rcx
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r11
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	iretq
.endm

MAKE_IRQ_STUB thorRtIsrIrq0, 0
MAKE_IRQ_STUB thorRtIsrIrq1, 1
MAKE_IRQ_STUB thorRtIsrIrq2, 2
MAKE_IRQ_STUB thorRtIsrIrq3, 3
MAKE_IRQ_STUB thorRtIsrIrq4, 4
MAKE_IRQ_STUB thorRtIsrIrq5, 5
MAKE_IRQ_STUB thorRtIsrIrq6, 6
MAKE_IRQ_STUB thorRtIsrIrq7, 7
MAKE_IRQ_STUB thorRtIsrIrq8, 8
MAKE_IRQ_STUB thorRtIsrIrq9, 9
MAKE_IRQ_STUB thorRtIsrIrq10, 10
MAKE_IRQ_STUB thorRtIsrIrq11, 11
MAKE_IRQ_STUB thorRtIsrIrq12, 12
MAKE_IRQ_STUB thorRtIsrIrq13, 13
MAKE_IRQ_STUB thorRtIsrIrq14, 14
MAKE_IRQ_STUB thorRtIsrIrq15, 15

# ---------------------------------------------------------
# Syscall stubs
# ---------------------------------------------------------

.section .text.stubs
.global syscallStub
syscallStub:
	# rsp still contains the user-space stack pointer
	# temporarily save it and switch to kernel-stack
	swapgs
	mov %rsp, %rbx
	mov %gs:.L_gsActiveExecutor, %rsp
	mov .L_executorKernelStack(%rsp), %rsp

	# syscall stores rip to rcx and rflags to r11
	push %r11 
	push %rcx
	push %rbx

	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rax
	push %rdx
	push %rsi
	push %rdi

	# debugging: disallow use of the FPU in kernel code
#	mov %cr0, %r15
#	or $8, %r15
#	mov %r15, %cr0

	mov %rsp, %rdi
	call handleSyscall

	# debugging: disallow use of the FPU in kernel code
#	mov %cr0, %r15
#	and $0xFFFFFFFFFFFFFFF7, %r15
#	mov %r15, %cr0
	
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rax
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	
	# prepare rcx and r11 for sysret
	pop %rbx
	pop %rcx
	pop %r11

	mov %rbx, %rsp
	# TODO: is this necessary? should r11 not already have the flag set?
	#or $.L_kRflagsIf, %r11 # enable interrupts
	swapgs
	sysretq

# ---------------------------------------------------------
# Executor related functions
# ---------------------------------------------------------

.text
.global forkExecutor
forkExecutor:
	mov %gs:.L_gsActiveExecutor, %rsi
	mov .L_executorImagePtr(%rsi), %rdi
	
	# save the fs segment
	mov $.L_msrIndexFsBase, %rcx
	rdmsr
	shl $32, %rdx
	or %rdx, %rax
	mov %rax, .L_imageClientFs(%rdi)
	
	# save the gs segment.
	# we are sure that this function is only called from system code
	# so that the client gs is currently swapped to the kernel MSR.
	mov $.L_msrIndexKernelGsBase, %rcx
	rdmsr
	shl $32, %rdx
	or %rdx, %rax
	mov %rax, .L_imageClientGs(%rdi)

	# only save the registers that are callee-saved by system v
	mov %rbx, .L_imageRbx(%rdi)
	mov %rbp, .L_imageRbp(%rdi)
	mov %r12, .L_imageR12(%rdi)
	mov %r13, .L_imageR13(%rdi)
	mov %r14, .L_imageR14(%rdi)
	mov %r15, .L_imageR15(%rdi)

	# save the cpu's extended state
	fxsaveq .L_imageFxSave(%rdi)
	
	# setup the state for the second return
	mov (%rsp), %rdx
	mov %rdx, .L_imageRip(%rdi)
	mov %cs, %rsi
	mov %rsi, .L_imageCs(%rdi)
	pushfq
	popq .L_imageRflags(%rdi)
	leaq 8(%rsp), %rcx
	mov %rcx, .L_imageRsp(%rdi)
	mov %ss, %rsi
	mov %rsi, .L_imageSs(%rdi)

	movq $0, .L_imageRax(%rdi)
	mov $1, %rax
	ret

# arguments: void *pointer
.section .text.stubs
.global _restoreExecutorRegisters
_restoreExecutorRegisters:
	# setup the IRET frame
	pushq .L_imageSs(%rdi)
	pushq .L_imageRsp(%rdi)
	pushq .L_imageRflags(%rdi)
	pushq .L_imageCs(%rdi)
	pushq .L_imageRip(%rdi)
	
	# restore the general purpose registers except for rdi
	mov .L_imageRax(%rdi), %rax
	mov .L_imageRbx(%rdi), %rbx
	mov .L_imageRcx(%rdi), %rcx
	mov .L_imageRdx(%rdi), %rdx
	mov .L_imageRsi(%rdi), %rsi
	mov .L_imageRbp(%rdi), %rbp

	mov .L_imageR8(%rdi), %r8
	mov .L_imageR9(%rdi), %r9
	mov .L_imageR10(%rdi), %r10
	mov .L_imageR11(%rdi), %r11
	mov .L_imageR12(%rdi), %r12
	mov .L_imageR13(%rdi), %r13
	mov .L_imageR14(%rdi), %r14
	mov .L_imageR15(%rdi), %r15
	
	# restore the cpu's extended state
	fxrstorq .L_imageFxSave(%rdi)
	
	mov .L_imageRdi(%rdi), %rdi
	iretq

