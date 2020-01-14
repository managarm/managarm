.set HOST_RSP, 0x00006c14
.set HOST_RIP, 0x00006c16


.section .text
.global  vmResume;
.global  vmLaunch;
//edi: launch state
vmLaunch:
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	pushq %rdi;
	mov $HOST_RSP, %rcx
	mov $HOST_RIP, %rdx

	vmwrite %rsp, %rcx;
	lea 1f(%rip), %rdi;
	vmwrite %rdi, %rdx;

	movq (%rsp), %rdi;

	movq 8(%rdi), %rbx;
	movq 16(%rdi), %rcx;
	movq 24(%rdi), %rdx;
	movq 32(%rdi), %rsi;
	movq 48(%rdi), %rbp;
	movq 56(%rdi), %r8;
	movq 64(%rdi), %r9;
	movq 72(%rdi), %r10;
	movq 80(%rdi), %r11;
	movq 88(%rdi), %r12;
	movq 96(%rdi), %r13;
	movq 104(%rdi), %r14;
	movq 112(%rdi), %r15;
	movq (%rdi), %rax;
	movq 40(%rdi), %rdi;
	sti;
	vmlaunch;
	1:
	pushq %rdi;
	movq 8(%rsp), %rdi;
	movq %rax, (%rdi);
	movq %rbx, 8(%rdi);
	movq %rcx, 16(%rdi);
	movq %rdx, 24(%rdi);
	movq %rsi, 32(%rdi);
	movq %rbp, 48(%rdi);
	movq %r8,  56(%rdi);
	movq %r9,  64(%rdi);
	movq %r10, 72(%rdi);
	movq %r11, 80(%rdi);
	movq %r12, 88(%rdi);
	movq %r13, 96(%rdi);
	movq %r14, 104(%rdi);
	movq %r15, 112(%rdi);
	popq %r8;
	popq %r9;
	movq %r8, 40(%r9);

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax

	retq

vmResume:
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	pushq %rdi;
	mov $HOST_RSP, %rcx
	mov $HOST_RIP, %rdx

	vmwrite %rsp, %rcx;
	lea 2f(%rip), %rdi;
	vmwrite %rdi, %rdx;

	movq (%rsp), %rdi;

	movq 8(%rdi), %rbx;
	movq 16(%rdi), %rcx;
	movq 24(%rdi), %rdx;
	movq 32(%rdi), %rsi;
	movq 48(%rdi), %rbp;
	movq 56(%rdi), %r8;
	movq 64(%rdi), %r9;
	movq 72(%rdi), %r10;
	movq 80(%rdi), %r11;
	movq 88(%rdi), %r12;
	movq 96(%rdi), %r13;
	movq 104(%rdi), %r14;
	movq 112(%rdi), %r15;
	movq (%rdi), %rax;
	movq 40(%rdi), %rdi;
	sti;
	vmresume;
	2:
	pushq %rdi;
	movq 8(%rsp), %rdi;
	movq %rax, (%rdi);
	movq %rbx, 8(%rdi);
	movq %rcx, 16(%rdi);
	movq %rdx, 24(%rdi);
	movq %rsi, 32(%rdi);
	movq %rbp, 48(%rdi);
	movq %r8,  56(%rdi);
	movq %r9,  64(%rdi);
	movq %r10, 72(%rdi);
	movq %r11, 80(%rdi);
	movq %r12, 88(%rdi);
	movq %r13, 96(%rdi);
	movq %r14, 104(%rdi);
	movq %r15, 112(%rdi);
	popq %r8;
	popq %r9;
	movq %r8, 40(%r9);

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax

	retq

