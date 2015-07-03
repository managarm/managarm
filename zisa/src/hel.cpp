
#include "../../frigg/include/types.hpp"
#include "../../hel/include/hel.h"

typedef uint64_t Word;

extern "C" Word syscall0(Word index);
extern "C" Word syscall1(Word index, Word arg0);
extern "C" Word syscall2(Word index, Word arg0, Word arg1);
extern "C" Word syscall3(Word index, Word arg0, Word arg1,
		Word arg2);
extern "C" Word syscall5(Word index, Word arg0, Word arg1,
		Word arg2, Word arg3, Word arg4, Word arg5);

HelError helLog(const char *string, size_t length) {
	register Word in_syscall asm ("rdi") = (Word)kHelCallLog;
	register Word in_string asm ("rsi") = (Word)string;
	register Word in_length asm ("rdx") = (Word)length;
	register Word out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_string), "r" (in_length)
		: "rcx", "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}

HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second) {
	register Word in_syscall asm ("rdi") = (Word)kHelCallCreateBiDirectionPipe;
	register Word out_error asm ("rdi");
	register Word out_first asm ("rsi");
	register Word out_second asm ("rdx");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_first), "=r" (out_second)
		: "r" (in_syscall)
		: "rcx", "r8", "r9", "rax", "rbx" );
	*first = (HelHandle)out_first;
	*second = (HelHandle)out_second;
	return (HelError)out_error;
}
HelError helRecvString(HelHandle handle, char *buffer, size_t length) {
	register Word in_syscall asm ("rdi") = (Word)kHelCallRecvString;
	register Word in_handle asm ("rsi") = (Word)handle;
	register Word in_buffer asm ("rdx") = (Word)buffer;
	register Word in_length asm ("rcx") = (Word)length;
	register Word out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_buffer), "r" (in_length)
		: "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}
HelError helSendString(HelHandle handle, const char *buffer, size_t length) {
	register Word in_syscall asm ("rdi") = (Word)kHelCallSendString;
	register Word in_handle asm ("rsi") = (Word)handle;
	register Word in_buffer asm ("rdx") = (Word)buffer;
	register Word in_length asm ("rcx") = (Word)length;
	register Word out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_buffer), "r" (in_length)
		: "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}

