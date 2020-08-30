#pragma once

#include <frigg/smart_ptr.hpp>
#include <frg/vector.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

struct IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(frigg::UnsafePtr<Thread> thread);

private:
	frg::vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor
