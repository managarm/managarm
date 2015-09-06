
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

.set .L_kGsGeneralState, 0x08
.set .L_kGsExtendedState, 0x18
.set .L_kGsFlags, 0x28

.set .L_kGsFlagAllowInts, 1

.set .L_kernelCodeSelector, 0x8
.set .L_kernelDataSelector, 0x10
.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

.global thorRtEntry
thorRtEntry:
	call thorMain
	ud2

.macro MAKE_FAULT_HANDLER name
.global faultStub\name
faultStub\name:
	pushq %rbx
	mov 8(%rsp), %rbx # rip
	pushq %rax
	pushq %rcx
	pushq %rdx
	pushq %rdi
	pushq %rsi
	pushq %rbp
	
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	
	mov %rbx, %rdi
	call handle\name\()Fault
	
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8

	popq %rbp
	popq %rsi
	popq %rdi
	popq %rdx
	popq %rcx
	popq %rax
	popq %rbx
	iretq
.endm

.macro MAKE_FAULT_HANDLER_WITHCODE name
.global faultStub\name
faultStub\name:
	popq %rdi # error code
	popq %rsi # rip

	call handle\name\()Fault
	ud2
.endm

MAKE_FAULT_HANDLER DivideByZero
MAKE_FAULT_HANDLER Debug
MAKE_FAULT_HANDLER Opcode
MAKE_FAULT_HANDLER_WITHCODE Double
MAKE_FAULT_HANDLER_WITHCODE Protection
MAKE_FAULT_HANDLER_WITHCODE Page

.macro MAKE_IRQ_HANDLER irq
.global thorRtIsrIrq\irq
thorRtIsrIrq\irq:
	pushq %rbx
	mov %gs:.L_kGsGeneralState, %rbx

	# check if we interrupted a thread
	cmp $0, %rbx
	je .L_nothread_irq\irq

	# save the registers in the thread structure
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

	popq .L_generalRbx(%rbx)
	popq .L_generalRip(%rbx)
	popq %rax # cs
	popq .L_generalRflags(%rbx)
	popq .L_generalRsp(%rbx)
	add $8, %rsp # skip ss

	# determine if we interrupted the kernel or userspace
	test $3, %rax
	setzb .L_generalKernel(%rbx)

	mov $\irq, %rdi
	push $restoreThisThread
	jmp thorIrq

.L_nothread_irq\irq:
	# this happens only while we are in the scheduler
	pushq %rax
	pushq %rcx
	pushq %rdx
	pushq %rdi
	pushq %rsi
	pushq %rbp
	
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	mov $\irq, %rdi
	call thorIrq
	
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8

	popq %rbp
	popq %rsi
	popq %rdi
	popq %rdx
	popq %rcx
	popq %rax

	popq %rbx
	iretq

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

# blocks the current thread. returns twice (like fork)
# returns 1 when the thread is blocked
# and 0 when the thread is continues execution
.global saveThisThread
saveThisThread:
	# system v abi says we can clobber rax and rcx
	mov %gs:.L_kGsGeneralState, %rax
	mov %gs:.L_kGsExtendedState, %rcx
	
	# only save the registers that are callee-saved by system v
	mov %rbx, .L_generalRbx(%rax)
	mov %rbp, .L_generalRbp(%rax)
	mov %r12, .L_generalR12(%rax)
	mov %r13, .L_generalR13(%rax)
	mov %r14, .L_generalR14(%rax)
	mov %r15, .L_generalR15(%rax)

	# save the cpu's extended state
	fxsaveq (%rcx)
	
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
	
	# restore the cpu's extended state
	mov %gs:.L_kGsExtendedState, %rcx
	fxrstor (%rcx)

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
	
	# enable/disable interrupts in rflags
	mov .L_generalRflags(%rbx), %rax
	or $.L_kRflagsIf, %rax
	testq $.L_kGsFlagAllowInts, %gs:.L_kGsFlags
	jnz .L_with_ints
	xor $.L_kRflagsIf, %rax

.L_with_ints:
	# check if we return to kernel mode
	testb $1, .L_generalKernel(%rbx)
	jnz .L_restore_kernel

	pushq $.L_userDataSelector
	pushq .L_generalRsp(%rbx)
	pushq %rax # rflags
	pushq $.L_userCode64Selector
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRax(%rbx), %rax
	mov .L_generalRbx(%rbx), %rbx
	iretq

.L_restore_kernel:
	pushq $.L_kernelDataSelector
	pushq .L_generalRsp(%rbx)
	pushq %rax # rflags
	pushq $.L_kernelCodeSelector
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRax(%rbx), %rax
	mov .L_generalRbx(%rbx), %rbx
	iretq

# enter user mode for the first time
.global enterUserMode
enterUserMode:
	pushq $.L_userDataSelector
	pushq %rdi # rsp
	pushq $0 # rflags
	pushq $.L_userCode64Selector
	pushq %rsi # rip
	iretq

