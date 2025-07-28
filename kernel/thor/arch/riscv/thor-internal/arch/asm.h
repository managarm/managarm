#pragma once

// This file is included from assembly.
// Use only preprocessor definitions here.

// tp (= AssemblyCpuData) offsets.
#define THOR_TP_SELF 0x0
#define THOR_TP_DOMAIN 0x8
#define THOR_TP_EXECUTOR 0x10
#define THOR_TP_EXCEPTION_STACK 0x18
#define THOR_TP_IRQ_STACK 0x20
#define THOR_TP_SCRATCH_SP 0x28
#define THOR_TP_ISEQ_PTR 0x30

#define THOR_EXECUTOR_UAR 0x10
