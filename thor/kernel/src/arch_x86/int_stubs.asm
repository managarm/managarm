
.set kRflagsBase, 0x1
.set kRflagsIf, 0x200

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

.global thorRtEntry
thorRtEntry:
	call thorMain
	hlt

.global thorRtHalt
thorRtHalt:
	hlt
	jmp thorRtHalt

.global thorRtLoadCs
thorRtLoadCs:
	movq %rsp, %rax
	movabs $reloadCsFinish, %rcx
	pushq $0
	pushq %rax
	pushfq
	pushq %rdi
	pushq %rcx
	iretq

reloadCsFinish:
	ret

.macro MAKE_FAULT_HANDLER name
.global thorRtIsr\name
thorRtIsr\name:
	pushq %rax
	pushq %rbx
	
	mov %gs:0x08, %rbx

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
	popq .L_generalRax(%rbx)
	popq .L_generalRip(%rbx)
	add $8, %rsp # skip cs
	popq .L_generalRflags(%rbx)
	popq .L_generalRsp(%rbx)
	add $8, %rsp # skip ss

	call thor\name
	jmp thorRtHalt
.endm

.global thorRtIsrDivideByZeroError
thorRtIsrDivideByZeroError:
	call thorDivideByZeroError
	jmp thorRtHalt

MAKE_FAULT_HANDLER InvalidOpcode

.global thorRtIsrDoubleFault
thorRtIsrDoubleFault:
	call thorDoubleFault
	jmp thorRtHalt

.global thorRtIsrGeneralProtectionFault
thorRtIsrGeneralProtectionFault:
	call thorGeneralProtectionFault
	jmp thorRtHalt

.global thorRtIsrPageFault
thorRtIsrPageFault:
	mov 16(%rsp), %rax
	and $3, %rax
	jz kernelPageFault

	mov %gs:0x08, %rbx
	
	popq %rsi # pop error code
	popq .L_generalRip(%rbx)
	add $8, %rsp # skip cs
	popq .L_generalRflags(%rbx)
	popq .L_generalRsp(%rbx)
	add $8, %rsp # skip ss
	
	mov %cr2, %rdi
	call thorUserPageFault
	jmp thorRtHalt

kernelPageFault:
	mov %cr2, %rdi
	popq %rdx # pop error code
	popq %rsi # pop faulting rip
	call thorKernelPageFault
	jmp thorRtHalt

.macro MAKE_IRQ_HANDLER irq
.global thorRtIsrIrq\irq
thorRtIsrIrq\irq:
	pushq %rbx
	mov %gs:0x08, %rbx

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
	call thorIrq
	jmp thorRtHalt
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
	# system v abi says we can clobber rax
	mov %gs:0x08, %rax
	
	# only save the registers that are callee-saved by system v
	mov %rbx, .L_generalRbx(%rax)
	mov %rbp, .L_generalRbp(%rax)
	mov %r12, .L_generalR12(%rax)
	mov %r13, .L_generalR13(%rax)
	mov %r14, .L_generalR14(%rax)
	mov %r15, .L_generalR15(%rax)
	
	# setup the state for the second return
	mov (%rsp), %rdx
	mov %rdx, .L_generalRip(%rax)
	mov %rsp, .L_generalRip(%rax)
	movq $0, .L_generalRax(%rax)

	mov $1, %rax
	ret

# restores a thread's state after an interrupt
# or after it has been blocked
.global restoreThisThread
restoreThisThread:
	mov %gs:0x08, %rbx

	mov .L_generalRax(%rbx), %rax
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
	
	# check if we return to kernel mode
	testb $1, .L_generalKernel(%rbx)
	jnz .L_restore_kernel

	pushq $0x23 # ss
	pushq .L_generalRsp(%rbx)
	pushq .L_generalRflags(%rbx)
	pushq $0x1B # cs
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRbx(%rbx), %rbx
	iretq

.L_restore_kernel:
	pushq $0x0 # ss
	pushq .L_generalRsp(%rbx)
	pushq .L_generalRflags(%rbx)
	pushq $0x08 # cs
	pushq .L_generalRip(%rbx)
	
	mov .L_generalRbx(%rbx), %rbx
	iretq

