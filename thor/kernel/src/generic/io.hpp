
namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(KernelUnsafePtr<Thread> thread);

private:
	frigg::Vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor

