#include <atomic>
#include <stdint.h>
#include <string.h>

#include <thor-internal/credentials.hpp>

namespace thor {

static std::atomic<uint64_t> globalCredentialId;

Credentials::Credentials() {
	// TODO: Generate real UUIDs instead of ascending numbers.
	uint64_t id = globalCredentialId.fetch_add(1, std::memory_order_relaxed) + 1;
	memset(_credentials, 0, 16);
	memcpy(_credentials + 8, &id, sizeof(uint64_t));
}

}
