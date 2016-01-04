
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
.set .L_kGsStateSize, 0x08
.set .L_kGsFlags, 0x20

.set .L_kernelCodeSelector, 0x8
.set .L_kernelDataSelector, 0x10
.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

# macro to construct an interrupt handler
.set .L_typeFaultNoCode, 1
.set .L_typeFaultWithCode, 2
.set .L_typeIrq, 3
.set .L_typeCall, 4

.macro MAKE_HANDLER type, name, func, number=0
.global \name
\name:
	# if there is no error code we push a random word to keep the stack aligned
.if \type != .L_typeFaultWithCode
	sub $8, %rsp
.endif
	# classical prologue
	# we need this as extended save area has variable size
	push %rbp
	mov %rsp, %rbp
	
	# allocate space for the state frame structs
	push %rax
	mov %gs:.L_kGsStateSize, %rax
	sub %rax, %rsp

	# save all general purpose registers except for rax and rbp
	mov %rbx, .L_frameRbx(%rsp)
	mov %rcx, .L_frameRcx(%rsp)
	mov %rdx, .L_frameRdx(%rsp)
	mov %rsi, .L_frameRsi(%rsp)
	mov %rdi, .L_frameRdi(%rsp)

	mov %r8, .L_frameR8(%rsp)
	mov %r9, .L_frameR9(%rsp)
	mov %r10, .L_frameR10(%rsp)
	mov %r11, .L_frameR11(%rsp)
	mov %r12, .L_frameR12(%rsp)
	mov %r13, .L_frameR13(%rsp)
	mov %r14, .L_frameR14(%rsp)
	mov %r15, .L_frameR15(%rsp)

	# save the cpu's extended state
	fxsaveq .L_frameFxSave(%rsp)
	
	# save all remaining registers
	mov -8(%rbp), %rax # frame rax
	mov   (%rbp), %rbx # frame rbp
	mov 16(%rbp), %rcx # frame rip
	mov 24(%rbp), %rdx # frame cs
	mov 32(%rbp), %rsi # frame rflags
	mov 40(%rbp), %rdi # frame rsp
	mov %rax, .L_frameRax(%rsp)
	mov %rbx, .L_frameRbp(%rsp)
	mov %rcx, .L_frameRip(%rsp)
	mov %rsi, .L_frameRflags(%rsp)
	mov %rdi, .L_frameRsp(%rsp)

	# determine if we interrupted the kernel or userspace
	test $3, %rdx
	setzb .L_frameKernel(%rsp)
	
	# call the handler function
	mov %rsp, %rdi
.if \type == .L_typeFaultWithCode
	mov 8(%rbp), %rsi
.elseif \type == .L_typeIrq
	mov $\number, %rsi
.endif
	call \func
	ud2
.endm

MAKE_HANDLER .L_typeFaultNoCode, faultStubDivideByZero, handleDivideByZeroFault
MAKE_HANDLER .L_typeFaultNoCode, faultStubDebug, handleDebugFault
MAKE_HANDLER .L_typeFaultNoCode, faultStubOpcode, handleOpcodeFault
MAKE_HANDLER .L_typeFaultWithCode, faultStubDouble, handleDoubleFault
MAKE_HANDLER .L_typeFaultWithCode, faultStubProtection, handleProtectionFault
MAKE_HANDLER .L_typeFaultWithCode, faultStubPage, handlePageFault

MAKE_HANDLER .L_typeIrq, thorRtIsrIrq0, thorIrq, number=0
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq1, thorIrq, number=1
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq2, thorIrq, number=2
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq3, thorIrq, number=3
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq4, thorIrq, number=4
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq5, thorIrq, number=5
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq6, thorIrq, number=6
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq7, thorIrq, number=7
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq8, thorIrq, number=8
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq9, thorIrq, number=9
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq10, thorIrq, number=10
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq11, thorIrq, number=11
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq12, thorIrq, number=12
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq13, thorIrq, number=13
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq14, thorIrq, number=14
MAKE_HANDLER .L_typeIrq, thorRtIsrIrq15, thorIrq, number=15

MAKE_HANDLER .L_typeCall, thorRtIsrPreempted, onPreemption

# saves the current state to a buffer. returns twice (like fork)
# returns 1 when the state is saved and 0 when it is restored
.global forkState
forkState:
	# only save the registers that are callee-saved by system v
	mov %rbx, .L_frameRbx(%rdi)
	mov %rbp, .L_frameRbp(%rdi)
	mov %r12, .L_frameR12(%rdi)
	mov %r13, .L_frameR13(%rdi)
	mov %r14, .L_frameR14(%rdi)
	mov %r15, .L_frameR15(%rdi)

	# save the cpu's extended state
	fxsaveq .L_frameFxSave(%rdi)
	
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

.global restoreStateFrame
restoreStateFrame:
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
	fxrstorq .L_frameFxSave(%rdi)

	# check if we return to kernel mode
	testb $1, .L_frameKernel(%rdi)
	jnz .L_restore_kernel_frame

	pushq $.L_userDataSelector
	pushq .L_frameRsp(%rdi)
	pushq .L_frameRflags(%rdi)
	pushq $.L_userCode64Selector
	pushq .L_frameRip(%rdi)
	
	mov .L_frameRdi(%rdi), %rdi
	iretq

.L_restore_kernel_frame:
	pushq $.L_kernelDataSelector
	pushq .L_frameRsp(%rdi)
	pushq .L_frameRflags(%rdi)
	pushq $.L_kernelCodeSelector
	pushq .L_frameRip(%rdi)
	
	mov .L_frameRdi(%rdi), %rdi
	iretq

