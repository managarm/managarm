
.set .L_kRflagsIf, 0x200

.set .L_kSyscallRsp, 0x00
.set .L_kSyscallRip, 0x08
.set .L_kSyscallRflags, 0x10
.set .L_kSyscallReturnRdi, 0x18
.set .L_kSyscallReturnRsi, 0x20
.set .L_kSyscallReturnRdx, 0x28
.set .L_kSyscallSavedRbp, 0x30
.set .L_kSyscallSavedR15, 0x38

.set .L_kSyscallFxSave, 0x40

.set .L_kAdditionalRax, 0x00
.set .L_kAdditionalRbx, 0x08
.set .L_kAdditionalRcx, 0x10
.set .L_kAdditionalRdx, 0x18
.set .L_kAdditionalRdi, 0x20
.set .L_kAdditionalRsi, 0x28
.set .L_kAdditionalRbp, 0x30
.set .L_kAdditionalR8, 0x38
.set .L_kAdditionalR9, 0x40
.set .L_kAdditionalR10, 0x48
.set .L_kAdditionalR11, 0x50
.set .L_kAdditionalR12, 0x58
.set .L_kAdditionalR13, 0x60
.set .L_kAdditionalR14, 0x68
.set .L_kAdditionalR15, 0x70

.set .L_kGsSyscallState, 0x18
.set .L_kGsSyscallStackPtr, 0x20
.set .L_kGsFlags, 0x28

.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

.global syscallStub
syscallStub:
	mov %gs:.L_kGsSyscallState, %rbx
	
	# syscall stores rip to rcx and rflags to r11
	# rsp still contains the user-space stack pointer
	mov %rcx, .L_kSyscallRip(%rbx)
	mov %r11, .L_kSyscallRflags(%rbx)
	mov %rsp, .L_kSyscallRsp(%rbx)

	# the following registers are not restored on a usual syscall return
	# they are preserved by the x86_64 calling convention so there is no need to restore them
	mov %r15, .L_kSyscallSavedR15(%rbx)
	mov %rbp, .L_kSyscallSavedRbp(%rbx)

	# save the cpu's extended state
	fxsaveq .L_kSyscallFxSave(%rbx)

	# switch to kernel stack and satisfy the x86_64 calling convention
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
	
.global jumpFromSyscall
jumpFromSyscall:
	mov %gs:.L_kGsSyscallState, %rbx
	
	# restore the cpu's extended state
	fxrstorq .L_kSyscallFxSave(%rbx)

	mov .L_kAdditionalRax(%rdi), %rax
	mov .L_kAdditionalRcx(%rdi), %rcx
	mov .L_kAdditionalRdx(%rdi), %rdx
	mov .L_kAdditionalRsi(%rdi), %rsi
	mov .L_kAdditionalRbp(%rdi), %rbp
	mov .L_kAdditionalR8(%rdi), %r8
	mov .L_kAdditionalR9(%rdi), %r9
	mov .L_kAdditionalR10(%rdi), %r10
	mov .L_kAdditionalR11(%rdi), %r11
	mov .L_kAdditionalR12(%rdi), %r12
	mov .L_kAdditionalR13(%rdi), %r13
	mov .L_kAdditionalR14(%rdi), %r14
	mov .L_kAdditionalR15(%rdi), %r15
	
	pushq $.L_userDataSelector
	pushq .L_kSyscallRsp(%rbx)
	pushq .L_kSyscallRflags(%rbx)
	pushq $.L_userCode64Selector
	pushq .L_kSyscallRip(%rbx)

	# note: we have to preserve rcx and r11 so we cannot use sysret
	mov .L_kAdditionalRbx(%rdi), %rbx
	mov .L_kAdditionalRdi(%rdi), %rdi
	iretq

