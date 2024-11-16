#pragma once

#include <smarter.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

struct BindableHandle { };

// A dummy counter for smarter::shared_ptr that allows you to
// construct eternal shared pointers without allocating.
struct EternalCounter final : smarter::counter {
	EternalCounter()
	: counter{smarter::adopt_rc, nullptr, 1} { }

	void dispose() override {
		panicLogger() << "thor: Disposing an EternalCounter!" << frg::endlog;
	}

protected:
	~EternalCounter() = default;
};

} // namespace thor
