
#include <thor-internal/io.hpp>

namespace thor {

// --------------------------------------------------------
// IoSpace
// --------------------------------------------------------

IoSpace::IoSpace() : p_ports(*kernelAlloc) { }

void IoSpace::addPort(uintptr_t port) {
	p_ports.push(port);
}

void IoSpace::enableInThread(frigg::UnsafePtr<Thread> thread) {
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->getContext().enableIoPort(p_ports[i]);
}

} // namespace thor

