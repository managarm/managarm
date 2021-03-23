// vim: ft=arm64asm

.macro MAKE_EXC_ENTRY name
.align 7
b \name
.endm

.align 11
.global thorExcVectors
thorExcVectors:
	MAKE_EXC_ENTRY el1InvalidExcStub // EL1t sync
	MAKE_EXC_ENTRY el1InvalidExcStub // EL1t irq
	MAKE_EXC_ENTRY el1InvalidExcStub // EL1t fiq
	MAKE_EXC_ENTRY el1InvalidExcStub // EL1t error

	MAKE_EXC_ENTRY el1SyncStub // EL1h sync
	MAKE_EXC_ENTRY el1IrqStub // EL1h irq
	MAKE_EXC_ENTRY el1InvalidExcStub // EL1h fiq
	MAKE_EXC_ENTRY el1SErrorStub // EL1h error

	MAKE_EXC_ENTRY el0SyncStub // EL0 sync
	MAKE_EXC_ENTRY el0IrqStub // EL0 irq
	MAKE_EXC_ENTRY el0InvalidExcStub  // EL0 fiq
	MAKE_EXC_ENTRY el0SErrorStub // EL0 error

	MAKE_EXC_ENTRY el0InvalidExcStub // EL0 AArch32 sync
	MAKE_EXC_ENTRY el0InvalidExcStub // EL0 AArch32 irq
	MAKE_EXC_ENTRY el0InvalidExcStub // EL0 AArch32 fiq
	MAKE_EXC_ENTRY el0InvalidExcStub // EL0 AArch32 error

.set .L_excStateSize, 304

.set .L_irqDomain, 0
.set .L_faultDomain, 1
.set .L_userDomain, 3

.set .L_currentDomainOff, 8
.set .L_exceptionStackOff, 16
.set .L_irqStackOff, 24

.macro MAKE_STUB name, handler, el, domain, stack
\name:
	msr tpidrro_el0, x0
	.if \el == 0
	mrs x0, tpidr_el1
	ldr x0, [x0, \stack]
	.else
	mov x0, sp
	.endif

	sub x0, x0, .L_excStateSize

	// Save x1-x30
	str x1, [x0, #8]
	stp x2, x3, [x0, #16]
	stp x4, x5, [x0, #32]
	stp x6, x7, [x0, #48]
	stp x8, x9, [x0, #64]
	stp x10, x11, [x0, #80]
	stp x12, x13, [x0, #96]
	stp x14, x15, [x0, #112]
	stp x16, x17, [x0, #128]
	stp x18, x19, [x0, #144]
	stp x20, x21, [x0, #160]
	stp x22, x23, [x0, #176]
	stp x24, x25, [x0, #192]
	stp x26, x27, [x0, #208]
	stp x28, x29, [x0, #224]
	str x30, [x0, #240]

	// Save old sp
	.if \el == 0
	mrs x1, sp_el0
	.else
	mov x1, sp
	.endif

	str x1, [x0, #248]

	// Set new stack
	mov sp, x0

	// Save x0
	mrs x0, tpidrro_el0
	str x0, [sp, #0]

	// Avoid leaking x0 contents to other threads
	mov x0, xzr
	msr tpidrro_el0, x0

	// Save exception state
	mrs x0, elr_el1
	str x0, [sp, #256]
	mrs x0, spsr_el1
	str x0, [sp, #264]
	mrs x0, esr_el1
	str x0, [sp, #272]
	mrs x0, far_el1
	str x0, [sp, #280]

	// Save domain
	mrs x0, tpidr_el1
	ldr x1, [x0, .L_currentDomainOff]
	str x1, [sp, #288]

	// Set new domain
	mov x1, \domain
	str x1, [x0, #8]

	// Save TPIDR_EL0
	mrs x0, tpidr_el0
	str x0, [sp, #296]

	// Enter handler
	mov x29, xzr
	mov x0, sp

	bl \handler

	// Restore TPIDR_EL0
	ldr x0, [sp, #296]
	msr tpidr_el0, x0

	// Restore domain
	mrs x0, tpidr_el1
	ldr x1, [sp, #288]
	str x1, [x0, #8]

	// Restore exception state
	ldr x0, [sp, #256]
	msr elr_el1, x0
	ldr x0, [sp, #264]
	msr spsr_el1, x0

	// Restore old stack (EL0)
	.if \el == 0
	ldr x1, [sp, #248]
	msr sp_el0, x1
	.endif

	// Restore x2-x30
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x19, [sp, #144]
	ldp x20, x21, [sp, #160]
	ldp x22, x23, [sp, #176]
	ldp x24, x25, [sp, #192]
	ldp x26, x27, [sp, #208]
	ldp x28, x29, [sp, #224]
	ldr x30, [sp, #240]

	mov x0, sp

	// Restore old stack (EL1)
	.if \el == 1
	ldr x1, [x0, #248]
	mov sp, x1
	.endif

	ldp x0, x1, [x0, #0]

	eret
.endm

MAKE_STUB el1InvalidExcStub, onPlatformInvalidException, 1, .L_faultDomain, .L_exceptionStackOff
MAKE_STUB el0InvalidExcStub, onPlatformInvalidException, 0, .L_faultDomain, .L_exceptionStackOff

MAKE_STUB el1SyncStub, onPlatformSyncFault, 1, .L_faultDomain, .L_exceptionStackOff
MAKE_STUB el1IrqStub, onPlatformIrq, 1, .L_irqDomain, .L_irqStackOff
MAKE_STUB el1SErrorStub, onPlatformAsyncFault, 1, .L_faultDomain, .L_exceptionStackOff

MAKE_STUB el0SyncStub, onPlatformSyncFault, 0, .L_faultDomain, .L_exceptionStackOff
MAKE_STUB el0IrqStub, onPlatformIrq, 0, .L_irqDomain, .L_irqStackOff
MAKE_STUB el0SErrorStub, onPlatformAsyncFault, 0, .L_faultDomain, .L_exceptionStackOff

.global workStub
workStub:
	stp x0, x1, [sp, #-16]!
	stp x2, x3, [sp, #-16]!
	stp x4, x5, [sp, #-16]!
	stp x6, x7, [sp, #-16]!
	stp x8, x9, [sp, #-16]!
	stp x10, x11, [sp, #-16]!
	stp x12, x13, [sp, #-16]!
	stp x14, x15, [sp, #-16]!
	stp x16, x17, [sp, #-16]!
	stp x18, x19, [sp, #-16]!
	stp x20, x21, [sp, #-16]!
	stp x22, x23, [sp, #-16]!
	stp x24, x25, [sp, #-16]!
	stp x26, x27, [sp, #-16]!
	stp x28, x29, [sp, #-16]!
	str x30, [sp, #-16]!

	mov x29, xzr
	bl onPlatformWork

	ldr x0, [sp, #256] // #256
	msr spsr_el1, x0
	ldr x0, [sp, #272] // #272
	msr elr_el1, x0
	ldr x0, [sp, #288] // #288
	msr sp_el0, x0
	ldr x0, [sp, #304] // #304
	mrs x1, tpidr_el1
	str x0, [x1, #8]

	ldr x30, [sp], #16
	ldp x28, x29, [sp], #16
	ldp x26, x27, [sp], #16
	ldp x24, x25, [sp], #16
	ldp x22, x23, [sp], #16
	ldp x20, x21, [sp], #16
	ldp x18, x19, [sp], #16
	ldp x16, x17, [sp], #16
	ldp x14, x15, [sp], #16
	ldp x12, x13, [sp], #16
	ldp x10, x11, [sp], #16
	ldp x8, x9, [sp], #16
	ldp x6, x7, [sp], #16
	ldp x4, x5, [sp], #16
	ldp x2, x3, [sp], #16
	ldp x0, x1, [sp], #16

	eret

.global _restoreExecutorRegisters
_restoreExecutorRegisters:
	ldr x1, [x0, #288]
	ldr x2, [x0, #248]
	cmp x1, .L_userDomain
	b.eq .L2
	mov sp, x2
	b .L1
.L2:
	msr sp_el0, x2
.L1:
	// Restore TPIDR_EL0
	ldr x1, [x0, #296]
	msr tpidr_el0, x1

	// Restore exception state
	ldr x1, [x0, #256]
	msr elr_el1, x1
	ldr x1, [x0, #264]
	msr spsr_el1, x1

	// Restore x0-x30
	ldp x2, x3, [x0, #16]
	ldp x4, x5, [x0, #32]
	ldp x6, x7, [x0, #48]
	ldp x8, x9, [x0, #64]
	ldp x10, x11, [x0, #80]
	ldp x12, x13, [x0, #96]
	ldp x14, x15, [x0, #112]
	ldp x16, x17, [x0, #128]
	ldp x18, x19, [x0, #144]
	ldp x20, x21, [x0, #160]
	ldp x22, x23, [x0, #176]
	ldp x24, x25, [x0, #192]
	ldp x26, x27, [x0, #208]
	ldp x28, x29, [x0, #224]
	ldr x30, [x0, #240]

	ldp x0, x1, [x0, #0]
	eret

.set .L_executorImagePtr, 0

.global doForkExecutor
doForkExecutor:
	ldr x0, [x0, .L_executorImagePtr]

	// Save callee saved registers (and LR and FP)
	str x19, [x0, #152]
	stp x20, x21, [x0, #160]
	stp x22, x23, [x0, #176]
	stp x24, x25, [x0, #192]
	stp x26, x27, [x0, #208]
	stp x28, x29, [x0, #224]
	str x30, [x0, #240]

	// Save ip = return addr to caller
	str x30, [x0, #256]

	// Save sp
	mov x4, sp
	str x4, [x0, #248]

	// Set new PSTATE
	mov x4, #5
	mrs x5, daif
	orr x4, x4, x5
	str x4, [x0, #264]

	// Set new domain
	mrs x4, tpidr_el1
	ldr x4, [x4, #8]
	str x4, [x0, #288]

	// Save TPIDR_EL0
	mrs x4, tpidr_el0
	str x4, [x0, #296]

	mov x0, x2
	blr x1
	udf 0

.global enableIntsAndHaltForever
enableIntsAndHaltForever:
	msr daifclr, #15
1:
	wfi
	b 1b

.global saveFpSimdRegisters
saveFpSimdRegisters:
	stp q0, q1, [x0, #304]
	stp q2, q3, [x0, #336]
	stp q4, q5, [x0, #368]
	stp q6, q7, [x0, #400]
	stp q8, q9, [x0, #432]
	stp q10, q11, [x0, #464]
	stp q12, q13, [x0, #496]
	stp q14, q15, [x0, #528]
	stp q16, q17, [x0, #560]
	stp q18, q19, [x0, #592]
	stp q20, q21, [x0, #624]
	stp q22, q23, [x0, #656]
	stp q24, q25, [x0, #688]
	stp q26, q27, [x0, #720]
	stp q28, q29, [x0, #752]
	stp q30, q31, [x0, #784]

	mrs x1, fpcr
	mrs x2, fpsr

	add x0, x0, #816
	stp x1, x2, [x0]

	ret

.global restoreFpSimdRegisters
restoreFpSimdRegisters:
	ldp q0, q1, [x0, #304]
	ldp q2, q3, [x0, #336]
	ldp q4, q5, [x0, #368]
	ldp q6, q7, [x0, #400]
	ldp q8, q9, [x0, #432]
	ldp q10, q11, [x0, #464]
	ldp q12, q13, [x0, #496]
	ldp q14, q15, [x0, #528]
	ldp q16, q17, [x0, #560]
	ldp q18, q19, [x0, #592]
	ldp q20, q21, [x0, #624]
	ldp q22, q23, [x0, #656]
	ldp q24, q25, [x0, #688]
	ldp q26, q27, [x0, #720]
	ldp q28, q29, [x0, #752]
	ldp q30, q31, [x0, #784]

	add x0, x0, #816
	ldp x1, x2, [x0]

	msr fpcr, x1
	msr fpsr, x2

	ret
