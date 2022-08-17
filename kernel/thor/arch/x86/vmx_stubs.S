.set HOST_RSP, 0x00006c14
.set HOST_RIP, 0x00006c16


.section .text

.extern vmxUpdateHostRsp
.global vmxVmRun
.global vmxDoVmExit

vmxVmRun: # rdi: thor::vmx::Vmcs* vm, rsi: Guest GPR State, rdx: launched
	pushq %rbx
	pushq %rbp
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	movq %rdx, %rbx # RBX is saved by functions

	pushq %rsi;

	mov %rsp, %rsi
	call vmxUpdateHostRsp # Cannot push from this point

	cmpq $1, %rbx # Cannot modify flags from this piont

	mov (%rsp), %rdi

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
	
	je 1f

	vmlaunch
	jmp vmxDoVmExit # When vmlaunch errors it doesn't go to HOST_RIP (vmxDoVmExit), so we have to nudge it

1:
	vmresume

vmxDoVmExit:
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
	popq %rbp
	popq %rbx

	retq