#ifndef HEL_STUBS_H
#	error Architecture header included directly, include hel-stubs.h instead
#endif

typedef uint64_t HelWord;

// note: the rcx and r11 registers are clobbered by syscall
// so we do not use them to pass parameters
// we also clobber rbx in kernel code

extern inline __attribute__ (( always_inline )) HelError helSyscall0(int number) {
	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall0_1(int number,
		HelWord *res0) {
	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall0_2(int number,
		HelWord *res0, HelWord *res1) {
	HelWord error;
	register HelWord out0 asm("rsi");
	register HelWord out1 asm("rdx");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0), "=r" (out1)
			: "D" (number)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1(int number,
		HelWord arg0) {
	register HelWord in0 asm("rsi") = arg0;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1_1(int number,
		HelWord arg0, HelWord *res0) {
	register HelWord in0 asm("rsi") = arg0;

	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number), "r" (in0)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1_2(int number,
		HelWord arg0, HelWord *res0, HelWord *res1) {
	register HelWord in0 asm("rsi") = arg0;

	HelWord error;
	register HelWord out0 asm("rsi");
	register HelWord out1 asm("rdx");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0), "=r"(out1)
			: "D" (number), "r" (in0)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2(int number,
		HelWord arg0, HelWord arg1) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0), "r" (in1)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2_1(int number,
		HelWord arg0, HelWord arg1, HelWord *res0) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;

	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number), "r" (in0), "r" (in1)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2_2(int number,
		HelWord arg0, HelWord arg1, HelWord *res0, HelWord *res1) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;

	HelWord error;
	register HelWord out0 asm("rsi");
	register HelWord out1 asm("rdx");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0), "=r"(out1)
			: "D" (number), "r" (in0), "r" (in1)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall3(int number,
		HelWord arg0, HelWord arg1, HelWord arg2) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall3_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord *res0) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;

	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall4(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;
	register HelWord in3 asm("r8") = arg3;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2), "r" (in3)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall4_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord *res0) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;
	register HelWord in3 asm("r8") = arg3;

	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2), "r" (in3)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall5(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;
	register HelWord in3 asm("r8") = arg3;
	register HelWord in4 asm("r9") = arg4;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall6(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4,
		HelWord arg5) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;
	register HelWord in3 asm("r8") = arg3;
	register HelWord in4 asm("r9") = arg4;
	register HelWord in5 asm("r10") = arg5;

	HelWord error;

	asm volatile ( "syscall" : "=D" (error)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4), "r" (in5)
			: "rcx", "r11", "rbx", "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall6_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4,
		HelWord arg5, HelWord *res0) {
	register HelWord in0 asm("rsi") = arg0;
	register HelWord in1 asm("rdx") = arg1;
	register HelWord in2 asm("rax") = arg2;
	register HelWord in3 asm("r8") = arg3;
	register HelWord in4 asm("r9") = arg4;
	register HelWord in5 asm("r10") = arg5;

	HelWord error;
	register HelWord out0 asm("rsi");

	asm volatile ( "syscall" : "=D" (error), "=r" (out0)
			: "D" (number), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4), "r" (in5)
			: "rcx", "r11", "rbx", "memory" );

	*res0 = out0;
	return error;
}
