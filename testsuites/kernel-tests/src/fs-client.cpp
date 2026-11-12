#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <async/algorithm.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/client.hpp>

#include "testsuite.hpp"

namespace {

// Answer one real FS request, or close its conversation without replying.
async::result<void> serveIo(
    helix::UniqueLane lane,
    bool write,
    managarm::fs::Errors error,
    int64_t count,
    bool close,
    std::string_view payload = {},
    bool malformed = false
) {
	auto [accept] = co_await helix_ng::exchangeMsgs(lane, helix_ng::accept());
	HEL_CHECK(accept.error());
	auto conversation = accept.descriptor();
	auto [request, credentials] = co_await helix_ng::exchangeMsgs(
	    conversation, helix_ng::recvInline(), helix_ng::extractCredentials()
	);
	HEL_CHECK(request.error());
	HEL_CHECK(credentials.error());
	if (write) {
		char data[16];
		auto [receive] =
		    co_await helix_ng::exchangeMsgs(conversation, helix_ng::recvBuffer(data, sizeof(data)));
		HEL_CHECK(receive.error());
		assert(receive.actualLength() == 7);
	}
	if (close)
		co_return;
	managarm::fs::SvrResponse response;
	response.set_error(error);
	response.set_size(count);
	auto bytes = malformed ? std::string(1, '\0') : response.SerializeAsString();
	if (write) {
		auto [send] = co_await helix_ng::exchangeMsgs(
		    conversation, helix_ng::sendBuffer(bytes.data(), bytes.size())
		);
		HEL_CHECK(send.error());
	} else {
		auto [send, data] = co_await helix_ng::exchangeMsgs(
		    conversation,
		    helix_ng::sendBuffer(bytes.data(), bytes.size()),
		    helix_ng::sendBuffer(payload.data(), payload.size())
		);
		HEL_CHECK(send.error());
		HEL_CHECK(data.error());
	}
}

async::result<void> testWrite(
    managarm::fs::Errors error,
    int64_t count,
    protocols::fs::Error expected,
    bool close = false,
    bool malformed = false
) {
	auto [local, remote] = helix::createStream();
	protocols::fs::File file{std::move(remote)};
	co_await async::when_all(
	    serveIo(std::move(local), true, error, count, close, {}, malformed),
	    async::lambda([&]() -> async::result<void> {
		    auto result = co_await file.writeSome("payload", 7);
		    if (expected == protocols::fs::Error::none) {
			    assert(result && *result == static_cast<size_t>(count));
		    } else {
			    assert(!result && result.error() == expected);
		    }
	    })()
	);
}

async::result<void> testRead(
    managarm::fs::Errors error,
    protocols::fs::Error expected,
    bool close = false,
    std::string_view payload = {},
    bool malformed = false
) {
	auto [local, remote] = helix::createStream();
	protocols::fs::File file{std::move(remote)};
	co_await async::when_all(
	    serveIo(std::move(local), false, error, payload.size(), close, payload, malformed),
	    async::lambda([&]() -> async::result<void> {
		    char data[16];
		    auto result = co_await file.readSome(data, sizeof(data), {});
		    if (expected == protocols::fs::Error::none) {
			    assert(result && *result == payload.size());
			    assert(!memcmp(data, payload.data(), payload.size()));
		    } else {
			    assert(!result && result.error() == expected);
		    }
	    })()
	);
}

} // namespace

DEFINE_TEST(fsClientWrite, ([] {
	            using namespace protocols::fs;
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, 0, Error::internalError, false, true),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, 3, Error::none),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, 0, Error::none),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, -1, Error::internalError),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, 8, Error::internalError),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::BROKEN_PIPE, 0, Error::brokenPipe),
	                helix::currentDispatcher
	            );
	            async::run(
	                testWrite(managarm::fs::Errors::SUCCESS, 0, Error::ioError, true),
	                helix::currentDispatcher
	            );
            }))

DEFINE_TEST(
    fsClientRead, ([] {
	    using namespace protocols::fs;
	    async::run(
	        testRead(managarm::fs::Errors::SUCCESS, Error::none, false, "payload"),
	        helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::SUCCESS, Error::endOfFile), helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::SUCCESS, Error::internalError, false, {}, true),
	        helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::BROKEN_PIPE, Error::brokenPipe), helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::INTERRUPTED, Error::interrupted),
	        helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::END_OF_FILE, Error::endOfFile), helix::currentDispatcher
	    );
	    async::run(
	        testRead(managarm::fs::Errors::SUCCESS, Error::ioError, true), helix::currentDispatcher
	    );
    })
)
