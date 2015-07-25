
namespace thor {

class Thread : public SharedBase<Thread, KernelAlloc> {
friend class ThreadQueue;
public:
	void setup(void (*user_entry)(uintptr_t), uintptr_t argument,
			void *user_stack_ptr);
	
	UnsafePtr<Universe, KernelAlloc> getUniverse();
	UnsafePtr<AddressSpace, KernelAlloc> getAddressSpace();
	
	void setUniverse(SharedPtr<Universe, KernelAlloc> &&universe);
	void setAddressSpace(SharedPtr<AddressSpace, KernelAlloc> &&address_space);

	void enableIoPort(uintptr_t port);
	
	void switchTo();

private:
	SharedPtr<Universe, KernelAlloc> p_universe;
	SharedPtr<AddressSpace, KernelAlloc> p_addressSpace;

	SharedPtr<Thread, KernelAlloc> p_nextInQueue;
	UnsafePtr<Thread, KernelAlloc> p_previousInQueue;

	ThorRtThreadState p_state;
	frigg::arch_x86::Tss64 p_tss;
};

class ThreadQueue {
public:
	bool empty();

	void addBack(SharedPtr<Thread, KernelAlloc> &&thread);
	
	SharedPtr<Thread, KernelAlloc> removeFront();
	SharedPtr<Thread, KernelAlloc> remove(UnsafePtr<Thread, KernelAlloc> thread);

private:
	SharedPtr<Thread, KernelAlloc> p_front;
	UnsafePtr<Thread, KernelAlloc> p_back;
};

} // namespace thor

