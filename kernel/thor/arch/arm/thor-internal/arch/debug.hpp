#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct DummyLogHandler : public LogHandler {
	void printChar(char c) override;
};

} // namespace thor
