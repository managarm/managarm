#include <thor-internal/arch/asm.h>

.global riscvHypervisorRun
.global riscvHypervisorRestore
.global riscvHypervisorInstRead
.global riscvHypervisorLoadFaultStub

.option norelax
.option arch, +h

// void riscvHypervisorRun(HelRiscv64VirtualizationRegs *regs);
riscvHypervisorRun:
	addi sp, sp, -0x90

	// Save the host sp to hscratch.
	sd sp, THOR_TP_HSCRATCH_SP(tp)

	// Save tp to sscratch.
	csrw sscratch, tp

	// Save the host registers to the stack.
	sd x1, 0x0(sp)
	sd x2, 0x8(sp)
	sd x3, 0x10(sp)
	sd x4, 0x18(sp)
	sd x8, 0x20(sp)
	sd x9, 0x28(sp)
	sd x18, 0x30(sp)
	sd x19, 0x38(sp)
	sd x20, 0x40(sp)
	sd x21, 0x48(sp)
	sd x22, 0x50(sp)
	sd x23, 0x58(sp)
	sd x24, 0x60(sp)
	sd x25, 0x68(sp)
	sd x26, 0x70(sp)
	sd x27, 0x78(sp)
	sd a0, 0x80(sp)
	// 0x88 = temporary save slot.

	// Restore the guest registers.
	ld x1, 0x8(a0)
	ld x2, 0x10(a0)
	ld x3, 0x18(a0)
	ld x4, 0x20(a0)
	ld x5, 0x28(a0)
	ld x6, 0x30(a0)
	ld x7, 0x38(a0)
	ld x8, 0x40(a0)
	ld x9, 0x48(a0)
	// x10 = a0, set last
	ld x11, 0x58(a0)
	ld x12, 0x60(a0)
	ld x13, 0x68(a0)
	ld x14, 0x70(a0)
	ld x15, 0x78(a0)
	ld x16, 0x80(a0)
	ld x17, 0x88(a0)
	ld x18, 0x90(a0)
	ld x19, 0x98(a0)
	ld x20, 0xa0(a0)
	ld x21, 0xa8(a0)
	ld x22, 0xb0(a0)
	ld x23, 0xb8(a0)
	ld x24, 0xc0(a0)
	ld x25, 0xc8(a0)
	ld x26, 0xd0(a0)
	ld x27, 0xd8(a0)
	ld x28, 0xe0(a0)
	ld x29, 0xe8(a0)
	ld x30, 0xf0(a0)
	ld x31, 0xf8(a0)
	ld x10, 0x50(a0)

	sret

.p2align 2
riscvHypervisorRestore:
	// Swap tp and sscratch.
	csrrw tp, sscratch, tp

	// Store the guest sp and restore the host sp from hscratch.
	sd sp, THOR_TP_SCRATCH_SP(tp)
	ld sp, THOR_TP_HSCRATCH_SP(tp)

	// Store a0 to the temporary stack slot.
	sd a0, 0x88(sp)
	// Restore original a0.
	ld a0, 0x80(sp)

	// Save the guest registers.
	sd x1, 0x8(a0)

	// Load the guest sp.
	ld x1, THOR_TP_SCRATCH_SP(tp)
	// Save the guest sp.
	sd x1, 0x10(a0)

	sd x3, 0x18(a0)
	sd x5, 0x28(a0)
	sd x6, 0x30(a0)
	sd x7, 0x38(a0)
	sd x8, 0x40(a0)
	sd x9, 0x48(a0)

	// Load the saved tp from sscratch.
	csrrw x5, sscratch, zero
	// Save the guest tp.
	sd x5, 0x20(a0)

	// Load back the saved a0.
	ld x5, 0x88(sp)
	// Save a0.
	sd x5, 0x50(a0)

	sd x11, 0x58(a0)
	sd x12, 0x60(a0)
	sd x13, 0x68(a0)
	sd x14, 0x70(a0)
	sd x15, 0x78(a0)
	sd x16, 0x80(a0)
	sd x17, 0x88(a0)
	sd x18, 0x90(a0)
	sd x19, 0x98(a0)
	sd x20, 0xa0(a0)
	sd x21, 0xa8(a0)
	sd x22, 0xb0(a0)
	sd x23, 0xb8(a0)
	sd x24, 0xc0(a0)
	sd x25, 0xc8(a0)
	sd x26, 0xd0(a0)
	sd x27, 0xd8(a0)
	sd x28, 0xe0(a0)
	sd x29, 0xe8(a0)
	sd x30, 0xf0(a0)
	sd x31, 0xf8(a0)

	// Restore the host registers from the stack.
	ld x1, 0x0(sp)
	ld x2, 0x8(sp)
	ld x3, 0x10(sp)
	ld x4, 0x18(sp)
	ld x8, 0x20(sp)
	ld x9, 0x28(sp)
	ld x18, 0x30(sp)
	ld x19, 0x38(sp)
	ld x20, 0x40(sp)
	ld x21, 0x48(sp)
	ld x22, 0x50(sp)
	ld x23, 0x58(sp)
	ld x24, 0x60(sp)
	ld x25, 0x68(sp)
	ld x26, 0x70(sp)
	ld x27, 0x78(sp)

	addi sp, sp, 0x90
	ret

// a0: host address
// a1: guest address
// a2: scause ptr
riscvHypervisorInstRead:
	hlvx.hu t0, (a1)

	andi t1, t0, 0b11
	slti t1, t1, 0b11
	bgtz t1, 2f

	// Normal instruction.
	sh t0, (a0)
	addi a1, a1, 2
	hlvx.hu t0, (a1)
	sh t0, 2(a0)

	li a0, 1
	ret

// Fault.
1:
	li a0, 0
	ret

// Compressed instruction.
2:
	sw t0, (a0)
	li a0, 1
	ret

.p2align 2
riscvHypervisorLoadFaultStub:
	csrr t1, scause
	sd t1, (a2)

3:	auipc t2, %pcrel_hi(1b)
	addi t2, t2, %pcrel_lo(3b)
	csrw sepc, t2

	sret

.section .note.GNU-stack,"",%progbits
