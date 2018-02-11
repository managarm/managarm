
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include "signalfd.hpp"

namespace {

struct OpenFile : ProxyFile {
	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static async::result<size_t> ptRead(std::shared_ptr<void> object,
			void *buffer, size_t length) {
		auto self = static_cast<OpenFile *>(object.get());
		return self->readSome(buffer, length);
	}
	
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withRead(&ptRead);

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	OpenFile()
	: ProxyFile{nullptr} { }

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		throw std::runtime_error("read() from signalfd is not implemented");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t sequence) override, ([=] {
		std::cout << "Fix signalfd::poll()" << std::endl;
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

} // anonymous namespace

std::shared_ptr<ProxyFile> createSignalFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

