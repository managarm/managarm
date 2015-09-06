
.set .L_kRflagsIf, 0x200

.set .L_kSyscallRbp, 0x00
.set .L_kSyscallR12, 0x08
.set .L_kSyscallR13, 0x10
.set .L_kSyscallR14, 0x18
.set .L_kSyscallR15, 0x20
.set .L_kSyscallRsp, 0x28
.set .L_kSyscallRip, 0x30
.set .L_kSyscallRflags, 0x38

.set .L_kGsSyscallState, 0x10
.set .L_kGsSyscallStackPtr, 0x20
.set .L_kGsFlags, 0x28

.set .L_kGsFlagEnableInts, 1

.global syscallStub
syscallStub:
	mov %gs:.L_kGsSyscallState, %rbx
	
	mov %rbp, .L_kSyscallRbp(%rbx)
	mov %r12, .L_kSyscallR12(%rbx)
	mov %r13, .L_kSyscallR13(%rbx)
	mov %r14, .L_kSyscallR14(%rbx)
	mov %r15, .L_kSyscallR15(%rbx)
	mov %rsp, .L_kSyscallRsp(%rbx)

	# syscall stores rip to rcx and rflags to r11
	mov %rcx, .L_kSyscallRip(%rbx)
	mov %r11, .L_kSyscallRflags(%rbx)

	mov %gs:.L_kGsSyscallStackPtr, %rsp
	
	# satisfy the system v calling convention
	push %r14
	push %r13
	push %r12
	push %r10
	mov %rax, %rcx

	call thorSyscall
	ud2

.global thorRtReturnSyscall1
.global thorRtReturnSyscall2
.global thorRtReturnSyscall3
thorRtReturnSyscall1:
thorRtReturnSyscall2:
thorRtReturnSyscall3:
	mov %gs:.L_kGsSyscallState, %rbx

	mov .L_kSyscallRbp(%rbx), %rbp
	mov .L_kSyscallR12(%rbx), %r12
	mov .L_kSyscallR13(%rbx), %r13
	mov .L_kSyscallR14(%rbx), %r14
	mov .L_kSyscallR15(%rbx), %r15
	mov .L_kSyscallRsp(%rbx), %rsp

	# setup rcx and r11 for sysret
	mov .L_kSyscallRip(%rbx), %rcx
	mov .L_kSyscallRflags(%rbx), %r11
	
	# enable/disable interrupts in rflags
	or $.L_kRflagsIf, %r11
	testq $.L_kGsFlagEnableInts, %gs:.L_kGsFlags
	jnz .L_with_ints
	xor $.L_kRflagsIf, %r11

.L_with_ints:
	sysretq

