#pragma once

#include <thor-internal/debug.hpp>

typedef unsigned long SbiWord;

namespace thor {

struct FirmwareLogHandler : public LogHandler {
	void emit(Severity severity, frg::string_view msg) override;

	void printChar(char c);
	void sbiCall1(int ext, int func, SbiWord arg0);
};

} // namespace thor
