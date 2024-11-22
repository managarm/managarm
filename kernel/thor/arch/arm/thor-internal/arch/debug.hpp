#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct UartLogHandler : public LogHandler {
	constexpr UartLogHandler() {
		takesUrgentLogs = true;
	}

	void emit(frg::string_view record) override;
	void emitUrgent(frg::string_view record) override;

	void printChar(char c);
};

} // namespace thor
