
namespace thor {

class Thread : public SharedObject {
friend class ThreadQueue;
public:
	void setup(void (*user_entry)(uintptr_t), uintptr_t argument,
			void *user_stack_ptr);
	
	UnsafePtr<Universe> getUniverse();
	UnsafePtr<AddressSpace> getAddressSpace();
	
	void setUniverse(SharedPtr<Universe> &&universe);
	void setAddressSpace(SharedPtr<AddressSpace> &&address_space);

	void enableIoPort(uintptr_t port);
	
	void switchTo();

private:
	SharedPtr<Universe> p_universe;
	SharedPtr<AddressSpace> p_addressSpace;

	SharedPtr<Thread> p_nextInQueue;
	UnsafePtr<Thread> p_previousInQueue;

	ThorRtThreadState p_state;
	frigg::arch_x86::Tss64 p_tss;
};

class ThreadQueue {
public:
	bool empty();

	void addBack(SharedPtr<Thread> &&thread);
	
	SharedPtr<Thread> removeFront();
	SharedPtr<Thread> remove(UnsafePtr<Thread> thread);

private:
	SharedPtr<Thread> p_front;
	UnsafePtr<Thread> p_back;
};

} // namespace thor

