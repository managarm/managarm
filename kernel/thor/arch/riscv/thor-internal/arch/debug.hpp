#pragma once

#include <thor-internal/debug.hpp>

typedef unsigned long SbiWord;

namespace thor {

struct FirmwareLogHandler : public LogHandler {
	void emit(frg::string_view record) override;

	void printChar(char c);
	void sbiCall1(int ext, int func, SbiWord arg0);
};

} // namespace thor
