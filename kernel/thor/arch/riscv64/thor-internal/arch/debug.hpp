#pragma once

#include <thor-internal/debug.hpp>

typedef unsigned long SbiWord;

namespace thor {

struct FirmwareLogHandler : public LogHandler {
	void printChar(char c) override;
	void sbiCall1(int ext, int func, SbiWord arg0);
};

} // namespace thor
