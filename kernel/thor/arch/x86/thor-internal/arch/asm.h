#pragma once

// This file is included from assembly.
// Use only preprocessor definitions here.

// GS segment (= AssemblyCpuData) offsets.
#define THOR_GS_SELF 0x0
#define THOR_GS_EXECUTOR 0x8
#define THOR_GS_SYSCALL_STACK 0x10
#define THOR_GS_ISEQ_PTR 0x18

#define THOR_EXECUTOR_UAR 0x18

// Number of IDT IRQ stubs.
#define THOR_NUM_IRQ_SLOTS 192
// Size of each IDT IRQ stub.
#define THOR_IRQ_STUB_SIZE 16

// svm::GprState offsets.
#define THOR_SVM_GPR_STATE_RBX 0x0
#define THOR_SVM_GPR_STATE_RCX 0x8
#define THOR_SVM_GPR_STATE_RDX 0x10
#define THOR_SVM_GPR_STATE_RSI 0x18
#define THOR_SVM_GPR_STATE_RDI 0x20
#define THOR_SVM_GPR_STATE_RBP 0x28
#define THOR_SVM_GPR_STATE_R8 0x30
#define THOR_SVM_GPR_STATE_R9 0x38
#define THOR_SVM_GPR_STATE_R10 0x40
#define THOR_SVM_GPR_STATE_R11 0x48
#define THOR_SVM_GPR_STATE_R12 0x50
#define THOR_SVM_GPR_STATE_R13 0x58
#define THOR_SVM_GPR_STATE_R14 0x60
#define THOR_SVM_GPR_STATE_R15 0x68
#define THOR_SVM_GPR_STATE_DR0 0x70
#define THOR_SVM_GPR_STATE_DR1 0x78
#define THOR_SVM_GPR_STATE_DR2 0x80
#define THOR_SVM_GPR_STATE_DR3 0x88
