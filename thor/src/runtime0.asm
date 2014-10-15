
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

.global thorRtContinueThread
thorRtContinueThread:
	pushq $0x1B
	pushq $0
	pushfq
	pushq %rdi
	pushq %rsi
	iretq

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

