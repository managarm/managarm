#pragma once

#include <format>

template<class... Args>
void logPanic(std::format_string<Args...> fmt, Args&&... args) {
	std::cerr << std::format(fmt, args...) << std::endl;
	__builtin_trap();
}
