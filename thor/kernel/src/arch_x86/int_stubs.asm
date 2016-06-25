
# fields of the state structs
.set .L_frameRax, 0x0
.set .L_frameRbx, 0x8
.set .L_frameRcx, 0x10
.set .L_frameRdx, 0x18
.set .L_frameRsi, 0x20
.set .L_frameRdi, 0x28
.set .L_frameRbp, 0x30

.set .L_frameR8, 0x38
.set .L_frameR9, 0x40
.set .L_frameR10, 0x48
.set .L_frameR11, 0x50
.set .L_frameR12, 0x58
.set .L_frameR13, 0x60
.set .L_frameR14, 0x68
.set .L_frameR15, 0x70

.set .L_frameRsp, 0x78
.set .L_frameRip, 0x80
.set .L_frameRflags, 0x88
.set .L_frameKernel, 0x90

.set .L_frameFxSave, 0xA0

# kernel gs segment fields
.set .L_kGsExecutorImage, 0x00

.set .L_kernelCodeSelector, 0x8
.set .L_kernelDataSelector, 0x10
.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

# macro to construct an interrupt handler
.set .L_typeFaultNoCode, 1
.set .L_typeFaultWithCode, 2
.set .L_typeCall, 3

.macro MAKE_FAULT_STUB type, name, func, number=0
.global \name
\name:
	# if there is no error code we push a fake one to keep the image struct intact
.if \type != .L_typeFaultWithCode
	push $0
.endif
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
	call \func

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

MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDivideByZero, handleDivideByZeroFault
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDebug, handleDebugFault
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubOpcode, handleOpcodeFault
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubNoFpu, handleNoFpuFault
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubDouble, handleDoubleFault
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubProtection, handleProtectionFault
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubPage, handlePageFault

# TODO: handle this as an IRQ
MAKE_FAULT_STUB .L_typeCall, thorRtIsrPreempted, onPreemption

.macro MAKE_IRQ_STUB name, number
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
	call handleIrq

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

.global forkExecutor
forkExecutor:
	mov %gs:.L_kGsExecutorImage, %rdi

	# only save the registers that are callee-saved by system v
	mov %rbx, .L_frameRbx(%rdi)
	mov %rbp, .L_frameRbp(%rdi)
	mov %r12, .L_frameR12(%rdi)
	mov %r13, .L_frameR13(%rdi)
	mov %r14, .L_frameR14(%rdi)
	mov %r15, .L_frameR15(%rdi)

	# save the cpu's extended state
	#fxsaveq .L_frameFxSave(%rdi)
	
	# setup the state for the second return
	pushfq
	popq .L_frameRflags(%rdi)
	mov (%rsp), %rdx
	mov %rdx, .L_frameRip(%rdi)
	leaq 8(%rsp), %rcx
	mov %rcx, .L_frameRsp(%rdi)
	movb $1, .L_frameKernel(%rdi)
	movq $0, .L_frameRax(%rdi)

	mov $1, %rax
	ret

.global restoreExecutor
restoreExecutor:
	mov %gs:.L_kGsExecutorImage, %rdi

	# restore the general purpose registers except for rdi
	mov .L_frameRax(%rdi), %rax
	mov .L_frameRbx(%rdi), %rbx
	mov .L_frameRcx(%rdi), %rcx
	mov .L_frameRdx(%rdi), %rdx
	mov .L_frameRsi(%rdi), %rsi
	mov .L_frameRbp(%rdi), %rbp

	mov .L_frameR8(%rdi), %r8
	mov .L_frameR9(%rdi), %r9
	mov .L_frameR10(%rdi), %r10
	mov .L_frameR11(%rdi), %r11
	mov .L_frameR12(%rdi), %r12
	mov .L_frameR13(%rdi), %r13
	mov .L_frameR14(%rdi), %r14
	mov .L_frameR15(%rdi), %r15
	
	# restore the cpu's extended state
	#fxrstorq .L_frameFxSave(%rdi)

	# check if we return to kernel mode
	testb $1, .L_frameKernel(%rdi)
	jnz .L_restore_kernel

	pushq $.L_userDataSelector
	pushq .L_frameRsp(%rdi)
	pushq .L_frameRflags(%rdi)
	pushq $.L_userCode64Selector
	pushq .L_frameRip(%rdi)
	
	mov .L_frameRdi(%rdi), %rdi
	iretq

.L_restore_kernel:
	pushq $.L_kernelDataSelector
	pushq .L_frameRsp(%rdi)
	pushq .L_frameRflags(%rdi)
	pushq $.L_kernelCodeSelector
	pushq .L_frameRip(%rdi)
	
	mov .L_frameRdi(%rdi), %rdi
	iretq


