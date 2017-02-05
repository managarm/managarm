#ifndef THOR_GENERIC_IO_HPP
#define THOR_GENERIC_IO_HPP

#include <frigg/smart_ptr.hpp>
#include <frigg/vector.hpp>
#include "thread.hpp"

namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(frigg::UnsafePtr<Thread> thread);

private:
	frigg::Vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor

#endif // THOR_GENERIC_IO_HPP
