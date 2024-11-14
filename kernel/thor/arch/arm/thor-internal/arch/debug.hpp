#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct UartLogHandler : public LogHandler {
	void emit(Severity severity, frg::string_view msg) override;

	void printChar(char c);
};

} // namespace thor
