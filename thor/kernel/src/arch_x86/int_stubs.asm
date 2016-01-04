
.set kRflagsBase, 0x1
.set .L_kRflagsIf, 0x200

.set .L_generalRax, 0x0
.set .L_generalRbx, 0x8
.set .L_generalRcx, 0x10
.set .L_generalRdx, 0x18
.set .L_generalRsi, 0x20
.set .L_generalRdi, 0x28
.set .L_generalRbp, 0x30

.set .L_generalR8, 0x38
.set .L_generalR9, 0x40
.set .L_generalR10, 0x48
.set .L_generalR11, 0x50
.set .L_generalR12, 0x58
.set .L_generalR13, 0x60
.set .L_generalR14, 0x68
.set .L_generalR15, 0x70

.set .L_generalRsp, 0x78
.set .L_generalRip, 0x80
.set .L_generalRflags, 0x88
.set .L_generalKernel, 0x90

.set .L_generalFxSave, 0xA0

# fields of the thor::StateFrame struct

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
.set .L_kGsGeneralState, 0x10
.set .L_kGsFlags, 0x28

.set .L_kernelCodeSelector, 0x8
.set .L_kernelDataSelector, 0x10
.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

.global thorRtEntry
thorRtEntry:
	# enable SSE support
	mov %cr0, %rax
	and $0xFFFFFFFFFFFFFFFB, %rax # disable EM
	or $2, %rax # enable MP
	mov %rax, %cr0

	mov %cr4, %rax
	or $0x200, %rax # enable OSFXSR
	or $0x400, %rax # enable OSXMMEXCPT
	mov %rax, %cr4

	call thorMain
	ud2

# saves the registers in the thread structure
# expects the thread general save state in %rbx
.macro SAVE_REGISTERS
	mov %rax, .L_generalRax(%rbx)
	mov %rcx, .L_generalRcx(%rbx)
	mov %rdx, .L_generalRdx(%rbx)
	mov %rsi, .L_generalRsi(%rbx)
	mov %rdi, .L_generalRdi(%rbx)
	mov %rbp, .L_generalRbp(%rbx)

	mov %r8, .L_generalR8(%rbx)
	mov %r9, .L_generalR9(%rbx)
	mov %r10, .L_generalR10(%rbx)
	mov %r11, .L_generalR11(%rbx)
	mov %r12, .L_generalR12(%rbx)
	mov %r13, .L_generalR13(%rbx)
	mov %r14, .L_generalR14(%rbx)
	mov %r15, .L_generalR15(%rbx)

	# save the cpu's extended state
	fxsaveq .L_generalFxSave(%rbx)
.endm

# saves the interrupt stack frame in the thread structure
# expects the thread general save state in %rbx
.macro SAVE_FRAME
	popq .L_generalRip(%rbx)
	popq %rax # cs
	popq .L_generalRflags(%rbx)
	popq .L_generalRsp(%rbx)
	add $8, %rsp # skip ss

	# determine if we interrupted the kernel or userspace
	test $3, %rax
	setzb .L_generalKernel(%rbx)
.endm

.macro MAKE_FAULT_HANDLER name, has_code
.global faultStub\name
faultStub\name:
	# if there is no error code we push a random word to keep the stack aligned
.ifeq \has_code
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
	setzb .L_generalKernel(%rsp)
	
	# call the handler function
	mov %rsp, %rdi
.if \has_code
	mov 8(%rbp), %rsi
.endif
	call handle\name\()Fault
	ud2

#----------------------------------------------
#	pushq %rbx
#	mov %gs:.L_kGsGeneralState, %rbx
#	SAVE_REGISTERS
#	popq .L_generalRbx(%rbx)
#	SAVE_FRAME

#	push $restoreThisThread
#	jmp handle\name\()Fault
.endm

MAKE_FAULT_HANDLER DivideByZero, 0
MAKE_FAULT_HANDLER Debug, 0
MAKE_FAULT_HANDLER Opcode, 0
MAKE_FAULT_HANDLER Double, 1
MAKE_FAULT_HANDLER Protection, 1
MAKE_FAULT_HANDLER Page, 1

.macro MAKE_IRQ_HANDLER irq
.global thorRtIsrIrq\irq
thorRtIsrIrq\irq:
	pushq %rbx
	mov %gs:.L_kGsGeneralState, %rbx
	SAVE_REGISTERS
	popq .L_generalRbx(%rbx)
	SAVE_FRAME

	mov $\irq, %rdi
	push $restoreThisThread
	jmp thorIrq
.endm

MAKE_IRQ_HANDLER 0
MAKE_IRQ_HANDLER 1
MAKE_IRQ_HANDLER 2
MAKE_IRQ_HANDLER 3
MAKE_IRQ_HANDLER 4
MAKE_IRQ_HANDLER 5
MAKE_IRQ_HANDLER 6
MAKE_IRQ_HANDLER 7
MAKE_IRQ_HANDLER 8
MAKE_IRQ_HANDLER 9
MAKE_IRQ_HANDLER 10
MAKE_IRQ_HANDLER 11
MAKE_IRQ_HANDLER 12
MAKE_IRQ_HANDLER 13
MAKE_IRQ_HANDLER 14
MAKE_IRQ_HANDLER 15

.global thorRtIsrPreempted
thorRtIsrPreempted:
	pushq %rbx
	mov %gs:.L_kGsGeneralState, %rbx
	SAVE_REGISTERS
	popq .L_generalRbx(%rbx)
	SAVE_FRAME

	push $restoreThisThread
	jmp onPreemption

# blocks the current thread. returns twice (like fork)
# returns 1 when the thread is blocked
# and 0 when the thread is continues execution
.global saveThisThread
saveThisThread:
	# system v abi says we can clobber rax
	mov %gs:.L_kGsGeneralState, %rax
	
	# only save the registers that are callee-saved by system v
	mov %rbx, .L_generalRbx(%rax)
	mov %rbp, .L_generalRbp(%rax)
	mov %r12, .L_generalR12(%rax)
	mov %r13, .L_generalR13(%rax)
	mov %r14, .L_generalR14(%rax)
	mov %r15, .L_generalR15(%rax)

	# save the cpu's extended state
	fxsaveq .L_generalFxSave(%rax)
	
	# setup the state for the second return
	pushfq
	popq .L_generalRflags(%rax)
	mov (%rsp), %rdx
	mov %rdx, .L_generalRip(%rax)
	leaq 8(%rsp), %rcx
	mov %rcx, .L_generalRsp(%rax)
	movb $1, .L_generalKernel(%rax)
	movq $0, .L_generalRax(%rax)

	mov $1, %rax
	ret

# restores a thread's state after an interrupt
# or after it has been blocked
.global restoreThisThread
restoreThisThread:
	mov %gs:.L_kGsGeneralState, %rbx
	
	mov .L_generalRcx(%rbx), %rcx
	mov .L_generalRdx(%rbx), %rdx
	mov .L_generalRsi(%rbx), %rsi
	mov .L_generalRdi(%rbx), %rdi
	mov .L_generalRbp(%rbx), %rbp

	mov .L_generalR8(%rbx), %r8
	mov .L_generalR9(%rbx), %r9
	mov .L_generalR10(%rbx), %r10
	mov .L_generalR11(%rbx), %r11
	mov .L_generalR12(%rbx), %r12
	mov .L_generalR13(%rbx), %r13
	mov .L_generalR14(%rbx), %r14
	mov .L_generalR15(%rbx), %r15
	
	# restore the cpu's extended state
	fxrstorq .L_generalFxSave(%rbx)

	# check if we return to kernel mode
	testb $1, .L_generalKernel(%rbx)
	jnz .L_restore_kernel

	pushq $.L_userDataSelector
	pushq .L_generalRsp(%rbx)
	pushq .L_generalRflags(%rbx)
	pushq $.L_userCode64Selector
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRax(%rbx), %rax
	mov .L_generalRbx(%rbx), %rbx
	iretq

.L_restore_kernel:
	pushq $.L_kernelDataSelector
	pushq .L_generalRsp(%rbx)
	pushq .L_generalRflags(%rbx)
	pushq $.L_kernelCodeSelector
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRax(%rbx), %rax
	mov .L_generalRbx(%rbx), %rbx
	iretq

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


# enter user mode for the first time
.global enterUserMode
enterUserMode:
	pushq $.L_userDataSelector
	pushq %rdi # rsp
	pushq $.L_kRflagsIf # rflags, enable interrupts
	pushq $.L_userCode64Selector
	pushq %rsi # rip
	iretq

