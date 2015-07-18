
.set kRflagsBase, 0x1
.set kRflagsIf, 0x200

.set kContextRax, 0x0
.set kContextRbx, 0x8
.set kContextRcx, 0x10
.set kContextRdx, 0x18
.set kContextRsi, 0x20
.set kContextRdi, 0x28
.set kContextRbp, 0x30

.set kContextR8, 0x38
.set kContextR9, 0x40
.set kContextR10, 0x48
.set kContextR11, 0x50
.set kContextR12, 0x58
.set kContextR13, 0x60
.set kContextR14, 0x68
.set kContextR15, 0x70

.set kContextRsp, 0x78
.set kContextRip, 0x80
.set kContextRflags, 0x88

.extern thorMain
.extern thorRtUserContext

.global thorRtEntry
thorRtEntry:
	movabs $0xFFFF800103000000, %rsp
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

.global thorRtIsrDoubleFault
thorRtIsrDoubleFault:
	call thorDoubleFault
	jmp thorRtHalt

.global thorRtIsrPageFault
thorRtIsrPageFault:
	movabs $thorRtUserContext, %rax
	mov (%rax), %rbx
	
	popq %rsi # pop error code
	popq kContextRip(%rbx)
	add $8, %rsp # skip cs
	popq kContextRflags(%rbx)
	popq kContextRsp(%rbx)
	add $8, %rsp # skip ss
	
	mov %cr2, %rdi
	call thorPageFault
	jmp thorRtHalt

.macro MAKE_IRQ_HANDLER irq
.global thorRtIsrIrq\irq
thorRtIsrIrq\irq:
	pushq %rax
	pushq %rbx
	
	movabs $thorRtUserContext, %rax
	mov (%rax), %rbx

	mov %rcx, kContextRcx(%rbx)
	mov %rdx, kContextRdx(%rbx)
	mov %rsi, kContextRsi(%rbx)
	mov %rdi, kContextRdi(%rbx)
	mov %rbp, kContextRbp(%rbx)

	mov %r8, kContextR8(%rbx)
	mov %r9, kContextR9(%rbx)
	mov %r10, kContextR10(%rbx)
	mov %r11, kContextR11(%rbx)
	mov %r12, kContextR12(%rbx)
	mov %r13, kContextR13(%rbx)
	mov %r14, kContextR14(%rbx)
	mov %r15, kContextR15(%rbx)
	
	popq kContextRbx(%rbx)
	popq kContextRax(%rbx)
	popq kContextRip(%rbx)
	add $8, %rsp # skip cs
	popq kContextRflags(%rbx)
	popq kContextRsp(%rbx)
	add $8, %rsp # skip ss

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

.global thorRtIsrSyscall
thorRtIsrSyscall:
	movabs $thorRtUserContext, %rax
	mov (%rax), %rbx

	mov %rbp, kContextRbp(%rbx)
	mov %r10, kContextR10(%rbx)
	mov %r11, kContextR11(%rbx)
	mov %r12, kContextR12(%rbx)
	mov %r13, kContextR13(%rbx)
	mov %r14, kContextR14(%rbx)
	mov %r15, kContextR15(%rbx)

	popq kContextRip(%rbx)
	add $8, %rsp # skip cs
	popq kContextRflags(%rbx)
	popq kContextRsp(%rbx)
	add $8, %rsp # skip ss

	call thorSyscall
	jmp thorRtHalt

.global thorRtFullReturn
thorRtFullReturn:
	movabs $thorRtUserContext, %rax
	mov (%rax), %rbx
	
	pushq $0x1B # ss
	pushq kContextRsp(%rbx)
	pushq kContextRflags(%rbx)
	pushq $0x13 # cs
	pushq kContextRip(%rbx)

	mov kContextRcx(%rbx), %rcx
	mov kContextRdx(%rbx), %rdx
	mov kContextRsi(%rbx), %rsi
	mov kContextRdi(%rbx), %rdi
	mov kContextRbp(%rbx), %rbp

	mov kContextR8(%rbx), %r8
	mov kContextR9(%rbx), %r9
	mov kContextR10(%rbx), %r10
	mov kContextR11(%rbx), %r11
	mov kContextR12(%rbx), %r12
	mov kContextR13(%rbx), %r13
	mov kContextR14(%rbx), %r14
	mov kContextR15(%rbx), %r15

	mov kContextRax(%rbx), %rax
	mov kContextRbx(%rbx), %rbx

	iretq

.global thorRtReturnSyscall1
.global thorRtReturnSyscall2
.global thorRtReturnSyscall3
thorRtReturnSyscall1:
thorRtReturnSyscall2:
thorRtReturnSyscall3:
	movabs $thorRtUserContext, %rax
	mov (%rax), %rbx

	mov kContextRbp(%rbx), %rbp
	mov kContextR10(%rbx), %r10
	mov kContextR11(%rbx), %r11
	mov kContextR12(%rbx), %r12
	mov kContextR13(%rbx), %r13
	mov kContextR14(%rbx), %r14
	mov kContextR15(%rbx), %r15
	
	pushq $0x1B # ss
	pushq kContextRsp(%rbx)
	pushq kContextRflags(%rbx)
	pushq $0x13 # cs
	pushq kContextRip(%rbx)
	iretq

