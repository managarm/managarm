
void *operator new (size_t size, void *pointer);
extern "C" void __cxa_pure_virtual();

extern "C" void thorRtHalt();

struct ThorRtThreadState {
	uint64_t rbx;	// offset 0
	uint64_t rbp;	// offset 0x8
	uint64_t r12;	// offset 0x10
	uint64_t r13;	// offset 0x18
	uint64_t r14;	// offset 0x20
	uint64_t r15;	// offset 0x28
	void *rsp;		// offset 0x30
};

extern "C" void thorRtLoadCs(uint16_t selector);
extern "C" void thorRtInvalidatePage(void *pointer);
extern "C" void thorRtInvalidateSpace();
extern "C" void thorRtSwitchThread(ThorRtThreadState *save_state,
		ThorRtThreadState *restore_state);

extern "C" void thorRtEnterUserThread(uint16_t cs_selector, void *rip);

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

	T *access() {
		return (T *)p_object;
	}
	T *operator-> () {
		return access();
	}
	T &operator* () {
		return *access();
	}

private:
	char p_object[sizeof(T)];
};

