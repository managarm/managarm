#pragma once

// This file is included from assembly.
// Use only preprocessor definitions here.

// TPIDR_EL1 (= AssemblyCpuData) offsets.
#define THOR_TP_SELF 0x0
#define THOR_TP_DOMAIN 0x8
#define THOR_TP_EXCEPTION_STACK 0x10
#define THOR_TP_IRQ_STACK 0x18
#define THOR_TP_CURRENT_UAR 0x20
#define THOR_TP_ISEQ_PTR 0x28
