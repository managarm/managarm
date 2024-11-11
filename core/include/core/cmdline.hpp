#pragma once

#include <async/result.hpp>
#include <frg/string.hpp>
#include <optional>

struct Cmdline {
	async::result<std::string> get();

	// return whether the driver should dump kernel logs
	async::result<bool> dumpKernelLogs(frg::string_view driver);

private:
	std::optional<std::string> cmdline_;
};
