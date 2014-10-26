
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

.global thorRtIsrSyscall
thorRtIsrSyscall:
	call thorSyscall
	iretq

