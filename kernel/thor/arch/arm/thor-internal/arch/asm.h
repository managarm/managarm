#pragma once

// This file is included from assembly.
// Use only preprocessor definitions here.

// TPIDR_EL1 (= AssemblyCpuData) offsets.
#define THOR_TP_SELF 0x0
#define THOR_TP_EXECUTOR 0x8
#define THOR_TP_ISEQ_PTR 0x10

// Executor offsets.
#define THOR_EXECUTOR_IMAGE 0x0
#define THOR_EXECUTOR_UAR 0x10

// Trap frame offsets.
#define THOR_FRAME_X0 0x0 // x1-x30 follows
#define THOR_FRAME_SP 0xF8
#define THOR_FRAME_ELR 0x100
#define THOR_FRAME_SPSR 0x108
#define THOR_FRAME_ESR 0x110
#define THOR_FRAME_FAR 0x118
#define THOR_FRAME_TPIDR_EL0 0x120

#define THOR_FRAME_SIZE 304

// FP register state offsets.
#define THOR_FPREGS_V0 0x0 // v1-v32 follows
#define THOR_FPREGS_FPCR 0x200
#define THOR_FPREGS_FPSR 0x208
