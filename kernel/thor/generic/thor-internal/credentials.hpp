#pragma once

namespace thor {

struct Credentials {
	Credentials();

	const char *credentials() {
		return _credentials;
	}
protected:
	char _credentials[16];
};

}
