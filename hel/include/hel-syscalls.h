
typedef uint64_t HelWord;

extern inline HelError helLog(const char *string, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallLog;
	register HelWord in_string asm ("rsi") = (HelWord)string;
	register HelWord in_length asm ("rdx") = (HelWord)length;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_string), "r" (in_length)
		: "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}

extern inline void helPanic(const char *string, size_t length) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallPanic;
	register HelWord in_string asm ("rsi") = (HelWord)string;
	register HelWord in_length asm ("rdx") = (HelWord)length;
	asm volatile ( "int $0x80" :
		: "r" (in_syscall), "r" (in_string), "r" (in_length)
		: "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
}


extern inline HelError helAllocateMemory(size_t size, HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallAllocateMemory;
	register HelWord in_size asm ("rsi") = (HelWord)size;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_handle)
		: "r" (in_syscall), "r" (in_size)
		: "rdx", "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*handle = out_handle;
	return (HelError)out_error;
}

extern inline HelError helMapMemory(HelHandle handle,
		void *pointer, size_t size, void **actual_pointer) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallMapMemory;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_pointer asm ("rdx") = (HelWord)pointer;
	register HelWord in_size asm ("rcx") = (HelWord)size;
	register HelWord out_error asm ("rdi");
	register HelWord out_actual_pointer asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error), "=r" (out_actual_pointer)
		: "r" (in_syscall), "r" (in_pointer), "r" (in_size)
		: "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*actual_pointer = (void *)out_actual_pointer;
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
		: "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*handle = out_handle;
	return (HelError)out_error;
}

extern inline HelError helExitThisThread() {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallExitThisThread;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall)
		: "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}


extern inline HelError helCreateEventHub(HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallCreateEventHub;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_handle)
		: "r" (in_syscall)
		: "rdx", "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*handle = out_handle;
	return (HelError)out_error;
}
extern inline HelError helWaitForEvents(HelHandle handle,
		struct HelEvent *list, size_t max_items, HelNanotime max_time,
		size_t *num_items) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallWaitForEvents;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_list asm ("rdx") = (HelWord)list;
	register HelWord in_max_items asm ("rcx") = (HelWord)max_items;
	register HelWord in_max_time asm ("r8") = (HelWord)max_time;
	register HelWord out_error asm ("rdi");
	register HelWord out_num_items asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error), "=r" (out_num_items)
		: "r" (in_syscall), "r" (in_handle), "r" (in_list),
			"r" (in_max_items), "r" (in_max_time)
		: "r9", "r10", "r11", "rax", "rbx", "memory" );
	*num_items = out_num_items;
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
		: "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*first = (HelHandle)out_first;
	*second = (HelHandle)out_second;
	return (HelError)out_error;
}
extern inline HelError helSendString(HelHandle handle,
		const uint8_t *buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSendString;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_buffer asm ("rdx") = (HelWord)buffer;
	register HelWord in_length asm ("rcx") = (HelWord)length;
	register HelWord in_msg_request asm ("r8") = (HelWord)msg_request;
	register HelWord in_msg_sequence asm ("r9") = (HelWord)msg_sequence;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle), "r" (in_buffer), "r" (in_length),
			"r" (in_msg_request), "r" (in_msg_sequence)
		: "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}
extern inline HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSubmitRecvString;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_hub_handle asm ("rdx") = (HelWord)hub_handle;
	register HelWord in_buffer asm ("rcx") = (HelWord)buffer;
	register HelWord in_max_length asm ("r8") = (HelWord)max_length;
	register HelWord in_filter_request asm ("r9") = (HelWord)filter_request;
	register HelWord in_filter_sequence asm ("r10") = (HelWord)filter_sequence;
	register HelWord in_submit_id asm ("r11") = (HelWord)submit_id;
	register HelWord in_submit_function asm ("r12") = (HelWord)submit_function;
	register HelWord in_submit_object asm ("r13") = (HelWord)submit_object;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle), "r" (in_hub_handle),
			"r" (in_buffer), "r" (in_max_length),
			"r" (in_filter_request), "r" (in_filter_sequence),
			"r" (in_submit_id), "r" (in_submit_function), "r" (in_submit_object)
		: "rax", "rbx", "memory" );
	return (HelError)out_error;
}

extern inline HelError helCreateServer(HelHandle *server_handle,
		HelHandle *client_handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallCreateServer;
	register HelWord out_error asm ("rdi");
	register HelWord out_server_handle asm ("rsi");
	register HelWord out_client_handle asm ("rdx");
	asm volatile ( "int $0x80" : "=r" (out_error),
			"=r" (out_server_handle), "=r" (out_client_handle)
		: "r" (in_syscall)
		: "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*server_handle = out_server_handle;
	*client_handle = out_client_handle;
	return (HelError)out_error;
}
extern inline HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSubmitAccept;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_hub_handle asm ("rdx") = (HelWord)hub_handle;
	register HelWord in_submit_id asm ("rcx") = (HelWord)submit_id;
	register HelWord in_submit_function asm ("r8") = (HelWord)submit_function;
	register HelWord in_submit_object asm ("r9") = (HelWord)submit_object;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle), "r" (in_hub_handle),
			"r" (in_submit_id), "r" (in_submit_function), "r" (in_submit_object)
		: "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}
extern inline HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSubmitConnect;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_hub_handle asm ("rdx") = (HelWord)hub_handle;
	register HelWord in_submit_id asm ("rcx") = (HelWord)submit_id;
	register HelWord in_submit_function asm ("r8") = (HelWord)submit_function;
	register HelWord in_submit_object asm ("r9") = (HelWord)submit_object;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle), "r" (in_hub_handle),
			"r" (in_submit_id), "r" (in_submit_function), "r" (in_submit_object)
		: "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}

extern inline HelError helAccessIrq(int number, HelHandle *handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallAccessIrq;
	register HelWord in_number asm ("rsi") = (HelWord)number;
	register HelWord out_error asm ("rdi");
	register HelWord out_handle asm ("rsi");
	asm volatile ( "int $0x80" : "=r" (out_error), "=r" (out_handle)
		: "r" (in_syscall), "r" (in_number)
		: "rdx", "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*handle = out_handle;
	return (HelError)out_error;
}
extern inline HelError helSubmitWaitForIrq(HelHandle handle,
		HelHandle hub_handle, int64_t submit_id,
		uintptr_t submit_function, uintptr_t submit_object) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallSubmitWaitForIrq;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord in_hub_handle asm ("rdx") = (HelWord)hub_handle;
	register HelWord in_submit_id asm ("rcx") = (HelWord)submit_id;
	register HelWord in_submit_function asm ("r8") = (HelWord)submit_function;
	register HelWord in_submit_object asm ("r9") = (HelWord)submit_object;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle), "r" (in_hub_handle),
			"r" (in_submit_id), "r" (in_submit_function), "r" (in_submit_object)
		: "r10", "r11", "rax", "rbx", "memory" );
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
		: "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	*handle = out_handle;
	return (HelError)out_error;
}
extern inline HelError helEnableIo(HelHandle handle) {
	register HelWord in_syscall asm ("rdi") = (HelWord)kHelCallEnableIo;
	register HelWord in_handle asm ("rsi") = (HelWord)handle;
	register HelWord out_error asm ("rdi");
	asm volatile ( "int $0x80" : "=r" (out_error)
		: "r" (in_syscall), "r" (in_handle)
		: "rdx", "rcx", "r8", "r9", "r10", "r11", "rax", "rbx", "memory" );
	return (HelError)out_error;
}

