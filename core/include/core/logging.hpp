#pragma once

#include <print>

template<class... Args>
[[noreturn]] void logPanic(std::format_string<Args...> fmt, Args&&... args) {
	std::println(std::cerr, fmt, std::forward<Args>(args)...);
	__builtin_trap();
}
