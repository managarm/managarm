
#pragma once

#include <protocols/kernlet/compiler.hpp>

std::vector<uint8_t> compileFafnir(const uint8_t *code, size_t size,
		const std::vector<BindType> &bind_types);

