
typedef uint64_t Word;

typedef uint64_t PhysicalAddr;
typedef uint64_t VirtualAddr;
typedef uint64_t VirtualOffset;

void *operator new (size_t size, void *pointer);
extern "C" void __cxa_pure_virtual();

extern "C" void thorRtHalt();

struct ThorRtThreadState {
	Word rax;		// offset 0
	Word rbx;		// offset 0x8
	Word rcx;		// offset 0x10
	Word rdx;		// offset 0x18
	Word rsi;		// offset 0x20
	Word rdi;		// offset 0x28
	Word rbp;		// offset 0x30

	Word r8;		// offset 0x38
	Word r9;		// offset 0x40
	Word r10;		// offset 0x48
	Word r11;		// offset 0x50
	Word r12;		// offset 0x58
	Word r13;		// offset 0x60
	Word r14;		// offset 0x68
	Word r15;		// offset 0x70
	
	Word rsp;		// offset 0x78
	Word rip;		// offset 0x80
	Word rflags;	// offset 0x88
};

extern ThorRtThreadState *thorRtUserContext;

extern "C" void thorRtLoadCs(uint16_t selector);
extern "C" void thorRtInvalidatePage(void *pointer);
extern "C" void thorRtInvalidateSpace();
extern "C" void thorRtSwitchThread(ThorRtThreadState *save_state,
		ThorRtThreadState *restore_state);

extern "C" void thorRtEnterUserThread(uint16_t cs_selector, void *rip);

extern "C" void thorRtReturnSyscall1(uint64_t out0);
extern "C" void thorRtReturnSyscall2(uint64_t out0, uint64_t out1);
extern "C" void thorRtReturnSyscall3(uint64_t out0, uint64_t out1, uint64_t out2);

extern "C" void thorRtThreadEntry();

extern "C" void thorRtIsrDoubleFault();
extern "C" void thorRtIsrPageFault();
extern "C" void thorRtIsrSyscall();

template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		new(p_object) T(thor::util::forward<Args>(args)...);
	}

	T *get() {
		return (T *)p_object;
	}
	T *operator-> () {
		return get();
	}
	T &operator* () {
		return *get();
	}

private:
	char p_object[sizeof(T)];
};

