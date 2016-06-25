
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

.set .L_gsActiveExecutor, 0x08

.set .L_executorSyscallStack, 0x08

.set .L_userCode64Selector, 0x2B
.set .L_userDataSelector, 0x23

.global syscallStub
syscallStub:
	# rsp still contains the user-space stack pointer
	# temporarily save it and switch to kernel-stack
	mov %rsp, %r15
	mov %gs:.L_gsActiveExecutor, %rsp
	mov .L_executorSyscallStack(%rsp), %rsp

	# syscall stores rip to rcx and rflags to r11
	push %r11 
	push %rcx
	push %r15

	push %rbp
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rax
	push %rdx
	push %rsi
	push %rdi

	# debugging: disallow use of the FPU in kernel code
#	mov %cr0, %r15
#	or $8, %r15
#	mov %r15, %cr0

	mov %rsp, %rdi
	call handleSyscall

	# debugging: disallow use of the FPU in kernel code
#	mov %cr0, %r15
#	and $0xFFFFFFFFFFFFFFF7, %r15
#	mov %r15, %cr0
	
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rax
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %rbp
	
	# prepare rcx and r11 for sysret
	pop %r15
	pop %rcx
	pop %r11

	mov %r15, %rsp
	or $.L_kRflagsIf, %r11 # enable interrupts
	sysretq

.global jumpFromSyscall
jumpFromSyscall:
	ud2

