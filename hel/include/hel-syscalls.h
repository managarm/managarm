
typedef uint64_t HelWord;

extern inline HelError helLog(const char *string, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallLog;
	register HelWord in_string asm ("rsi") = (HelWord)string;
	register HelWord in_length asm ("rdx") = (HelWord)length;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_string), "r" (in_length)
		: "rcx", "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}

extern inline void helPanic(const char *string, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallPanic;
	register HelWord in_string asm ("rsi") = (HelWord)string;
	register HelWord in_length asm ("rdx") = (HelWord)length;
	asm volatile ( "int $0x80" :
		: "r" (in_syscall), "r" (in_string), "r" (in_length)
		: "rcx", "r8", "r9", "rax", "rbx" );
}


extern inline HelError helAllocateMemory(size_t size, HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallAllocateMemory;
	register HelWord in_size asm ("rsi") = (HelWord)size;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_handle)
		: "r" (in_syscall), "r" (in_size)
		: "rdx", "rcx", "r8", "r9", "rax", "rbx" );
	*handle = out_handle;
	return (HelError)out_error;
}

extern inline HelError helMapMemory(HelHandle handle, void *pointer, size_t size) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallMapMemory;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_pointer asm ("rdx") = (HelWord)pointer;
	register HelWord in_size asm ("rcx") = (HelWord)size;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_pointer), "r" (in_size)
		: "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}


extern inline HelError helCreateThread(void (*entry)(uintptr_t),
		uintptr_t argument, void *stack_ptr, HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallCreateThread;
	register HelWord in_entry asm ("rsi") = (HelWord)entry;
	register HelWord in_argument asm ("rdx") = (HelWord)argument;
	register HelWord in_stack_ptr asm ("rcx") = (HelWord)stack_ptr;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_handle)
		: "r" (in_syscall), "r" (in_entry), "r" (in_argument),
			"r" (in_stack_ptr)
		: "r8", "r9", "rax", "rbx" );
	*handle = out_handle;
	return (HelError)out_error;
}

extern inline HelError helExitThisThread() {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallExitThisThread;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall)
		: "rsi", "rdx", "rcx", "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}


extern inline HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallCreateBiDirectionPipe;
	register HelWord out_error asm ("rdi");
	register HelWord out_first asm ("rsi");
	register HelWord out_second asm ("rdx");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_first), "=r" (out_second)
		: "r" (in_syscall)
		: "rcx", "r8", "r9", "rax", "rbx" );
	*first = (HelHandle)out_first;
	*second = (HelHandle)out_second;
	return (HelError)out_error;
}
extern inline HelError helRecvString(HelHandle handle, char *buffer, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallRecvString;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_buffer asm ("rdx") = (HelWord)buffer;
	register HelWord in_length asm ("rcx") = (HelWord)length;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_buffer), "r" (in_length)
		: "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}
extern inline HelError helSendString(HelHandle handle, const char *buffer, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSendString;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_buffer asm ("rdx") = (HelWord)buffer;
	register HelWord in_length asm ("rcx") = (HelWord)length;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_buffer), "r" (in_length)
		: "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}


extern inline HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallAccessIo;
	register HelWord in_port_array asm ("rsi") = (HelWord)port_array;
	register HelWord in_num_ports asm ("rdx") = (HelWord)num_ports;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error), "=r" (out_handle)
		: "r" (in_syscall), "r" (in_port_array), "r" (in_num_ports)
		: "rcx", "r8", "r9", "rax", "rbx" );
	*handle = out_handle;
	return (HelError)out_error;
}
extern inline HelError helEnableIo(HelHandle handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallEnableIo;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle)
		: "rdx", "rcx", "r8", "r9", "rax", "rbx" );
	return (HelError)out_error;
}

