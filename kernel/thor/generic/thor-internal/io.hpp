#pragma once

#include <frg/vector.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

struct IoSpace {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<IoSpace>, Error> create();

	IoSpace(CtorToken);

	std::expected<void, Error> addPort(uintptr_t port);

	void enableInThread(smarter::borrowed_ptr<Thread> thread);

private:
	frg::vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor
