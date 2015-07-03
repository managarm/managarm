
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

.global thorRtEntry
thorRtEntry:
	movabs $0xFFFF800100200000, %rsp
	.extern thorMain
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

.global thorRtSwitchThread
thorRtSwitchThread:
	# save thread state
	mov %rbx, 0x0(%rdi)
	mov %rbp, 0x8(%rdi)
	mov %r12, 0x10(%rdi)
	mov %r13, 0x18(%rdi)
	mov %r14, 0x20(%rdi)
	mov %r15, 0x28(%rdi)
	mov %rsp, 0x30(%rdi)

	# restore thread state
	mov 0x0(%rsi), %rbx
	mov 0x8(%rsi), %rbp
	mov 0x10(%rsi), %r12
	mov 0x18(%rsi), %r13
	mov 0x20(%rsi), %r14
	mov 0x28(%rsi), %r15
	mov 0x30(%rsi), %rsp
	ret

.global thorRtEnterUserThread
thorRtEnterUserThread:
	pushq $0x1B
	pushq $0
	pushfq
	pushq %rdi
	pushq %rsi
	iretq

.global thorRtThreadEntry
thorRtThreadEntry:
	mov $0x13, %rdi
	mov %rbx, %rsi
	jmp thorRtEnterUserThread

.global thorRtIsrDoubleFault
thorRtIsrDoubleFault:
	call thorDoubleFault
	jmp thorRtHalt

.global thorRtIsrPageFault
thorRtIsrPageFault:
	call thorPageFault
	jmp thorRtHalt

.extern thorRtUserContext

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

	pop kContextRip(%rbx)
	add $8, %rsp # skip cs
	pop kContextRflags(%rbx)
	pop kContextRsp(%rbx)
	add $8, %rsp # skip ss

	call thorSyscall
	jmp thorRtHalt

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
	
	push $0x1B # ss
	push kContextRsp(%rbx)
	push kContextRflags(%rbx)
	push $0x13 # cs
	push kContextRip(%rbx)
	iretq

