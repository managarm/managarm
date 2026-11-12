#include <thor-internal/arch/asm.h>

.section .text

.global svmVmRun
svmVmRun:
	pushq %rbx # Save Host State
	pushq %rbp
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

	movq THOR_SVM_GPR_STATE_DR0(%rdi), %rax
	movq %rax, %dr0
	movq THOR_SVM_GPR_STATE_DR1(%rdi), %rax
	movq %rax, %dr1
	movq THOR_SVM_GPR_STATE_DR2(%rdi), %rax
	movq %rax, %dr2
	movq THOR_SVM_GPR_STATE_DR3(%rdi), %rax
	movq %rax, %dr3

	movq THOR_SVM_GPR_STATE_RBX(%rdi), %rbx
	movq THOR_SVM_GPR_STATE_RCX(%rdi), %rcx
	movq THOR_SVM_GPR_STATE_RDX(%rdi), %rdx
	movq THOR_SVM_GPR_STATE_RSI(%rdi), %rsi
	movq THOR_SVM_GPR_STATE_RBP(%rdi), %rbp
	movq THOR_SVM_GPR_STATE_R8(%rdi), %r8
	movq THOR_SVM_GPR_STATE_R9(%rdi), %r9
	movq THOR_SVM_GPR_STATE_R10(%rdi), %r10
	movq THOR_SVM_GPR_STATE_R11(%rdi), %r11
	movq THOR_SVM_GPR_STATE_R12(%rdi), %r12
	movq THOR_SVM_GPR_STATE_R13(%rdi), %r13
	movq THOR_SVM_GPR_STATE_R14(%rdi), %r14
	movq THOR_SVM_GPR_STATE_R15(%rdi), %r15
	
	pushq %rdi
	movq THOR_SVM_GPR_STATE_RDI(%rdi), %rdi
	
	# Get VMCB
	movq 0x8(%rsp), %rax

	vmload %rax
	# With V_INTR_MASKING, host interrupts cause exits only if IF is set at VMRUN.
	sti
	vmrun %rax
	cli
	vmsave %rax

	pushq %rdi # Save Guest RDI
	movq 0x8(%rsp), %rdi # Restore RDI from stack

	# Save Guest State
	movq %rbx, THOR_SVM_GPR_STATE_RBX(%rdi)
	movq %rcx, THOR_SVM_GPR_STATE_RCX(%rdi)
	movq %rdx, THOR_SVM_GPR_STATE_RDX(%rdi)
	movq %rsi, THOR_SVM_GPR_STATE_RSI(%rdi)
	movq %rbp, THOR_SVM_GPR_STATE_RBP(%rdi)
	movq %r8, THOR_SVM_GPR_STATE_R8(%rdi)
	movq %r9, THOR_SVM_GPR_STATE_R9(%rdi)
	movq %r10, THOR_SVM_GPR_STATE_R10(%rdi)
	movq %r11, THOR_SVM_GPR_STATE_R11(%rdi)
	movq %r12, THOR_SVM_GPR_STATE_R12(%rdi)
	movq %r13, THOR_SVM_GPR_STATE_R13(%rdi)
	movq %r14, THOR_SVM_GPR_STATE_R14(%rdi)
	movq %r15, THOR_SVM_GPR_STATE_R15(%rdi)

	movq %dr0, %rax
	movq %rax, THOR_SVM_GPR_STATE_DR0(%rdi)
	movq %dr1, %rax
	movq %rax, THOR_SVM_GPR_STATE_DR1(%rdi)
	movq %dr2, %rax
	movq %rax, THOR_SVM_GPR_STATE_DR2(%rdi)
	movq %dr3, %rax
	movq %rax, THOR_SVM_GPR_STATE_DR3(%rdi)

	# Restore host state
	popq %r8
	popq %r9
	mov %r8, THOR_SVM_GPR_STATE_RDI(%r9)

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
	popq %rbp
	popq %rbx

	ret

	.section .note.GNU-stack,"",%progbits
