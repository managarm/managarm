#pragma once

#include <array>
#include <expected>

#include <smarter.hpp>
#include <thor-internal/error.hpp>

namespace thor {

struct Credentials {
	Credentials();

	std::array<char, 16> credentials() {
		return _credentials;
	}
protected:
	std::array<char, 16> _credentials;
};

struct TokenObject final : Credentials {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<TokenObject>, Error> create();

	TokenObject(CtorToken) { }
};

}
