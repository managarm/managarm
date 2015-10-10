
.set .L_kRflagsIf, 0x200

.set .L_kSyscallRsp, 0x00
.set .L_kSyscallRip, 0x08
.set .L_kSyscallRflags, 0x10
.set .L_kSyscallReturnRdi, 0x18
.set .L_kSyscallReturnRsi, 0x20
.set .L_kSyscallReturnRdx, 0x28

.set .L_kSyscallFxSave, 0x30

.set .L_kGsSyscallState, 0x10
.set .L_kGsSyscallStackPtr, 0x18
.set .L_kGsFlags, 0x20

.global syscallStub
syscallStub:
	mov %gs:.L_kGsSyscallState, %rbx
	
	# syscall stores rip to rcx and rflags to r11
	mov %rcx, .L_kSyscallRip(%rbx)
	mov %r11, .L_kSyscallRflags(%rbx)
	
	# rsp still contains the user-space stack pointer
	mov %rsp, .L_kSyscallRsp(%rbx)

	# save the cpu's extended state
	fxsaveq .L_kSyscallFxSave(%rbx)

	# switch to kernel stack and satisfy the system v calling convention
	mov %gs:.L_kGsSyscallStackPtr, %rsp
	push %r14
	push %r13
	push %r12
	push %r10
	mov %rax, %rcx

	call thorSyscall
	
	# switch back to user-space stack and restore return arguments
	mov .L_kSyscallRsp(%rbx), %rsp
	mov .L_kSyscallReturnRdi(%rbx), %rdi
	mov .L_kSyscallReturnRsi(%rbx), %rsi
	mov .L_kSyscallReturnRdx(%rbx), %rdx
	
	# restore the cpu's extended state
	fxrstorq .L_kSyscallFxSave(%rbx)

	# setup rcx and r11 for sysret
	mov .L_kSyscallRip(%rbx), %rcx
	mov .L_kSyscallRflags(%rbx), %r11
	or $.L_kRflagsIf, %r11 # enable interrupts
	
	sysretq

