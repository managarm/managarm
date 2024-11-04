#pragma once

#include <array>

namespace thor {

struct Credentials {
	Credentials();

	std::array<char, 16> credentials() { return _credentials; }

  protected:
	std::array<char, 16> _credentials;
};

} // namespace thor
