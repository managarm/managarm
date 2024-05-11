#ifndef HEL_STUBS_H
#	error Architecture header included directly, include hel-stubs.h instead
#endif

typedef uint64_t HelWord;

extern inline __attribute__ (( always_inline )) HelError helSyscall0(int number) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall0_1(int number,
		HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code)
			: "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall0_2(int number,
		HelWord *res0, HelWord *res1) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord out0 asm("x1");
	register HelWord out1 asm("x2");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0), "=r" (out1)
			: "r" (code)
			: "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1(int number,
		HelWord arg0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1_1(int number,
		HelWord arg0, HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code), "r" (in0)
			: "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall1_2(int number,
		HelWord arg0, HelWord *res0, HelWord *res1) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord out0 asm("x1");
	register HelWord out1 asm("x2");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0), "=r"(out1)
			: "r" (code), "r" (in0)
			: "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2(int number,
		HelWord arg0, HelWord arg1) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0), "r" (in1)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2_1(int number,
		HelWord arg0, HelWord arg1, HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code), "r" (in0), "r" (in1)
			: "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall2_2(int number,
		HelWord arg0, HelWord arg1, HelWord *res0, HelWord *res1) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord out0 asm("x1");
	register HelWord out1 asm("x2");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0), "=r"(out1)
			: "r" (code), "r" (in0), "r" (in1)
			: "memory" );

	*res0 = out0;
	*res1 = out1;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall3(int number,
		HelWord arg0, HelWord arg1, HelWord arg2) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall3_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2)
			: "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall4(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord in3 asm("x4") = arg3;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2), "r" (in3)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall4_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord in3 asm("x4") = arg3;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2), "r" (in3)
			: "memory" );

	*res0 = out0;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall5(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord in3 asm("x4") = arg3;
	register HelWord in4 asm("x5") = arg4;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall6(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4,
		HelWord arg5) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord in3 asm("x4") = arg3;
	register HelWord in4 asm("x5") = arg4;
	register HelWord in5 asm("x6") = arg5;

	asm volatile ( "svc 0" : "=r" (error)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4), "r" (in5)
			: "memory" );

	return error;
}

extern inline __attribute__ (( always_inline )) HelError helSyscall6_1(int number,
		HelWord arg0, HelWord arg1, HelWord arg2, HelWord arg3, HelWord arg4,
		HelWord arg5, HelWord *res0) {
	register HelWord error asm("x0");
	register HelWord code asm("x0") = number;
	register HelWord in0 asm("x1") = arg0;
	register HelWord in1 asm("x2") = arg1;
	register HelWord in2 asm("x3") = arg2;
	register HelWord in3 asm("x4") = arg3;
	register HelWord in4 asm("x5") = arg4;
	register HelWord in5 asm("x6") = arg5;
	register HelWord out0 asm("x1");

	asm volatile ( "svc 0" : "=r" (error), "=r" (out0)
			: "r" (code), "r" (in0), "r" (in1), "r" (in2), "r" (in3), "r" (in4), "r" (in5)
			: "memory" );

	*res0 = out0;
	return error;
}
