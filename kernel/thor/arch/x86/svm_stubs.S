.section .text

.global vmRun
vmRun:
	# Save Host State
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
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
	
	# Restore Guest State
	movq %dr0, %rax
	pushq %rax
	movq %dr1, %rax
	pushq %rax
	movq %dr2, %rax
	pushq %rax
	movq %dr3, %rax
	pushq %rax

	pushq %rsi

	movq 0x78(%rdi), %rax
	movq %rax, %dr0
	movq 0x80(%rdi), %rax
	movq %rax, %dr1
	movq 0x88(%rdi), %rax
	movq %rax, %dr2
	movq 0x90(%rdi), %rax
	movq %rax, %dr3
	movq 0x70(%rdi), %rax
	movq %rax, %cr2

	movq 0x0(%rdi), %rbx
	movq 0x8(%rdi), %rcx
	movq 0x10(%rdi), %rdx
	movq 0x18(%rdi), %rsi
	movq 0x28(%rdi), %rbp
	movq 0x30(%rdi), %r8
	movq 0x38(%rdi), %r9
	movq 0x40(%rdi), %r10
	movq 0x48(%rdi), %r11
	movq 0x50(%rdi), %r12
	movq 0x58(%rdi), %r13
	movq 0x60(%rdi), %r14
	movq 0x68(%rdi), %r15
	
	pushq %rdi
	movq 0x20(%rdi), %rdi
	
	# Get VMCB
	movq 0x8(%rsp), %rax

	vmload %rax
	vmrun %rax
	vmsave %rax

	pushq %rdi # Save Guest RDI
	movq 0x8(%rsp), %rdi # Restore RDI from stack

	# Save Guest State
	movq %rbx, 0x0(%rdi)
	movq %rcx, 0x8(%rdi)
	movq %rdx, 0x10(%rdi)
	movq %rsi, 0x18(%rdi)
	movq %rbp, 0x28(%rdi)
	movq %r8, 0x30(%rdi)
	movq %r9, 0x38(%rdi)
	movq %r10, 0x40(%rdi)
	movq %r11, 0x48(%rdi)
	movq %r12, 0x50(%rdi)
	movq %r13, 0x58(%rdi)
	movq %r14, 0x60(%rdi)
	movq %r15, 0x68(%rdi)

	movq %dr0, %rax
	movq %rax, 0x78(%rdi)
	movq %dr1, %rax
	movq %rax, 0x80(%rdi)
	movq %dr2, %rax    
	movq %rax, 0x88(%rdi)
	movq %dr3, %rax
	movq %rax, 0x90(%rdi)
	movq %cr2, %rax
	movq %rax, 0x70(%rdi)


	# Restore host state
	popq %r8
	popq %r9
	mov %r8, 0x20(%r9)

	popq %rsi

	popq %rax
	movq %rax, %dr0
	popq %rax
	movq %rax, %dr1
	popq %rax
	movq %rax, %dr2
	popq %rax
	movq %rax, %dr3

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
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax

	ret
