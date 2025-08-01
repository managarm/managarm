#include <thor-internal/arch/asm.h>

.option norelax

// Emit debugging information + unwind tables.
.cfi_sections .eh_frame, .debug_frame

.global thorExceptionEntry
.global thorHandleException

.p2align 2
thorExceptionEntry:
	# This is a "signal frame", i.e., the return address must *not* be adjust to find
	# a call instruction that invoked this function.
	.cfi_startproc
	.cfi_signal_frame
	# RISC-V ABI recommends to use DWARF register number 64
	# as a dummy register for signal return addresses.
	.cfi_return_column 64

	# Swap tp and sscratch.
	# If sscratch was non-zero, we interrupted user mode.
	# Note: This allows us to test whether we interrupted kernel mode without loading sstatus.
	#       It also does not require us to immediately clobber a register for that purpose.
	csrrw tp, sscratch, tp
	bnez tp, 1f

	# Kernel-mode traps take this path.

	# Get back our kernel tp.
	csrr tp, sscratch

	# Store the previos SP in CpuData.
	sd sp, THOR_TP_SCRATCH_SP(tp)

	# Keep using the same stack. TODO: Detect double faults etc?

	j 2f

1:
	# User-mode traps continue here.

	# Store the previos SP in CpuData.
	sd sp, THOR_TP_SCRATCH_SP(tp)

	# Set SP to the executor's kernel stack.
	ld sp, THOR_TP_EXCEPTION_STACK(tp)

2:
	# Both user + kernel traps continue here.

	addi sp, sp, -0x108 # Size of frame.
	.cfi_def_cfa sp, 0

	sd           x1,  0x0(sp) # ra
	.cfi_offset  x1,  0x0
	# x2 = sp is saved separately.
	sd           x3, 0x10(sp) # gp
	.cfi_offset  x3, 0x10
	# x4 = tp is saved separately.
	sd           x5, 0x20(sp) # t0
	.cfi_offset  x5, 0x20
	sd           x6, 0x28(sp) # t1
	.cfi_offset  x6, 0x28
	sd           x7, 0x30(sp) # t2
	.cfi_offset  x7, 0x30
	sd           x8, 0x38(sp) # s0
	.cfi_offset  x8, 0x38
	sd           x9, 0x40(sp) # s1
	.cfi_offset  x9, 0x40
	sd          x10, 0x48(sp) # a0
	.cfi_offset x10, 0x48
	sd          x11, 0x50(sp) # a1
	.cfi_offset x11, 0x50
	sd          x12, 0x58(sp) # a2
	.cfi_offset x12, 0x58
	sd          x13, 0x60(sp) # a3
	.cfi_offset x13, 0x60
	sd          x14, 0x68(sp) # a4
	.cfi_offset x14, 0x68
	sd          x15, 0x70(sp) # a5
	.cfi_offset x15, 0x70
	sd          x16, 0x78(sp) # a6
	.cfi_offset x16, 0x78
	sd          x17, 0x80(sp) # a7
	.cfi_offset x17, 0x80
	sd          x18, 0x88(sp) # s2
	.cfi_offset x18, 0x88
	sd          x19, 0x90(sp) # s3
	.cfi_offset x19, 0x90
	sd          x20, 0x98(sp) # s4
	.cfi_offset x20, 0x98
	sd          x21, 0xA0(sp) # s5
	.cfi_offset x21, 0xA0
	sd          x22, 0xA8(sp) # s6
	.cfi_offset x22, 0xA8
	sd          x23, 0xB0(sp) # s7
	.cfi_offset x23, 0xB0
	sd          x24, 0xB8(sp) # s8
	.cfi_offset x24, 0xB8
	sd          x25, 0xC0(sp) # s9
	.cfi_offset x25, 0xC0
	sd          x26, 0xC8(sp) # s10
	.cfi_offset x26, 0xC8
	sd          x27, 0xD0(sp) # s11
	.cfi_offset x27, 0xD0
	sd          x28, 0xD8(sp) # t3
	.cfi_offset x28, 0xD8
	sd          x29, 0xE0(sp) # t4
	.cfi_offset x29, 0xE0
	sd          x30, 0xE8(sp) # t5
	.cfi_offset x30, 0xE8
	sd          x31, 0xF0(sp) # t6
	.cfi_offset x31, 0xF0

	# Write the previous tp to the frame, clear sscratch to zero.
	csrrw t0, sscratch, zero
	sd t0, 0x18(sp)
	.cfi_offset tp, 0x18

	# Write the previous pc to the frame
	csrr t0, sepc
	sd t0, 0xF8(sp)
	.cfi_offset 64, 0xF8

	# Write the previous sp to the frame.
	ld t0, THOR_TP_SCRATCH_SP(tp)
	sd t0, 0x8(sp)
	.cfi_offset sp, 0x8

	mv a0, sp
	call thorHandleException

	# Write the previous pc back to sepc (in case there was a nested exception).
	ld t0, 0xF8(sp)
	csrw sepc, t0

	# Load GPRs.
	ld           x1,  0x0(sp) # ra
	# Restore x2 (aka sp) last.
	ld           x3, 0x10(sp) # gp
	ld           x4, 0x18(sp) # tp
	ld           x5, 0x20(sp) # t0
	ld           x6, 0x28(sp) # t1
	ld           x7, 0x30(sp) # t2
	ld           x8, 0x38(sp) # s0
	ld           x9, 0x40(sp) # s1
	ld          x10, 0x48(sp) # a0
	ld          x11, 0x50(sp) # a1
	ld          x12, 0x58(sp) # a2
	ld          x13, 0x60(sp) # a3
	ld          x14, 0x68(sp) # a4
	ld          x15, 0x70(sp) # a5
	ld          x16, 0x78(sp) # a6
	ld          x17, 0x80(sp) # a7
	ld          x18, 0x88(sp) # s2
	ld          x19, 0x90(sp) # s3
	ld          x20, 0x98(sp) # s4
	ld          x21, 0xA0(sp) # s5
	ld          x22, 0xA8(sp) # s6
	ld          x23, 0xB0(sp) # s7
	ld          x24, 0xB8(sp) # s8
	ld          x25, 0xC0(sp) # s9
	ld          x26, 0xC8(sp) # s10
	ld          x27, 0xD0(sp) # s11
	ld          x28, 0xD8(sp) # t3
	ld          x29, 0xE0(sp) # t4
	ld          x30, 0xE8(sp) # t5
	ld          x31, 0xF0(sp) # t6
	# Finally, restore sp.
	ld           sp,  0x8(sp)
	sret
	.cfi_endproc

.global thorRestoreExecutorRegs

# ABI:
# a0: Pointer to frame.
.p2align 2
thorRestoreExecutorRegs:
	ld  x1,  0x0(a0)
	ld  x2,  0x8(a0)
	ld  x3, 0x10(a0)
	ld  x4, 0x18(a0)
	ld  x5, 0x20(a0)
	ld  x6, 0x28(a0)
	ld  x7, 0x30(a0)
	ld  x8, 0x38(a0)
	ld  x9, 0x40(a0)
	# Restore x10 (aka a0) last.
	ld x11, 0x50(a0)
	ld x12, 0x58(a0)
	ld x13, 0x60(a0)
	ld x14, 0x68(a0)
	ld x15, 0x70(a0)
	ld x16, 0x78(a0)
	ld x17, 0x80(a0)
	ld x18, 0x88(a0)
	ld x19, 0x90(a0)
	ld x20, 0x98(a0)
	ld x21, 0xA0(a0)
	ld x22, 0xA8(a0)
	ld x23, 0xB0(a0)
	ld x24, 0xB8(a0)
	ld x25, 0xC0(a0)
	ld x26, 0xC8(a0)
	ld x27, 0xD0(a0)
	ld x28, 0xD8(a0)
	ld x29, 0xE0(a0)
	ld x30, 0xE8(a0)
	ld x31, 0xF0(a0)
	# Finally, restore x10.
	ld x10, 0x48(a0)
	sret

.global doForkExecutor

# ABI:
# a0: Executor pointer.
# a1: Function pointer.
# a2: Argument for function.
.p2align 2
doForkExecutor:
	# Load pointer to frame.
	# TODO: Move this into C++ code.
	ld a0, (a0)

	# Store sstatus (see comments in Frame struct).
	# TODO: This may need to be revised for FPU / SIMD state.
	li t0, 0x100 # SPP = 1, SPIE = 0.
	sd t0, 0x100(a0)

	sd  x2,  0x8(a0) # Store sp.
	sd  x8, 0x38(a0) # Store s0.
	sd  x9, 0x40(a0) # Store s1.
	# Store s2 to s11.
	sd x18, 0x88(a0)
	sd x19, 0x90(a0)
	sd x20, 0x98(a0)
	sd x21, 0xA0(a0)
	sd x22, 0xA8(a0)
	sd x23, 0xB0(a0)
	sd x24, 0xB8(a0)
	sd x25, 0xC0(a0)
	sd x26, 0xC8(a0)
	sd x27, 0xD0(a0)

	# We continue at the return address.
	sd ra, 0xF8(a0)

	# Jump to the lambda.
	mv a0, a2
	jr a1
