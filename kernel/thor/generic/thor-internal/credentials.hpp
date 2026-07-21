#pragma once

#include <array>
#include <expected>

#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {

struct Credentials : RcuProtected {
	std::array<char, 16> credentials() {
		return _credentials;
	}
protected:
	// Streams embed Credentials by value; such embedded instances never enter descriptors.
	friend struct Stream;

	Credentials();

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
