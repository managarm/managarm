
#include <thor-internal/io.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

// --------------------------------------------------------
// IoSpace
// --------------------------------------------------------

std::expected<smarter::shared_ptr<IoSpace>, Error> IoSpace::create() {
	auto ptr = allocate_rcu_shared<IoSpace>(*kernelAlloc, CtorToken{});
	return ptr;
}

IoSpace::IoSpace(CtorToken) : p_ports(*kernelAlloc) { }

std::expected<void, Error> IoSpace::addPort(uintptr_t port) {
	if (port > 0xFFFF)
		return std::unexpected{Error::outOfBounds};
	p_ports.push(port);
	return {};
}

void IoSpace::enableInThread(smarter::borrowed_ptr<Thread> thread) {
#ifdef THOR_ARCH_SUPPORTS_PIO
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->getContext().enableIoPort(p_ports[i]);
#else
	(void)thread;
#endif
}

} // namespace thor

