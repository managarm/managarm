#pragma once

#include <frg/cmdline.hpp>
#include <frg/string.hpp>

namespace eir {

void extendCmdline(frg::string_view chunk);

std::span<frg::string_view> getCmdline();

inline void parseCmdline(std::span<frg::option> options) {
	auto chunks = getCmdline();
	for (frg::string_view chunk : chunks) {
		frg::parse_arguments(chunk, options);
	}
}

} // namespace eir
