#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct UartLogHandler : public LogHandler {
	void printChar(char c) override;
	void setPriority(Severity prio) override;
	void resetPriority() override;
};

} // namespace thor
