
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "signalfd.hpp"

namespace {

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
				&File::fileOperations);
	}

	OpenFile()
	: File{StructName::get("signalfd")} { }

	COFIBER_ROUTINE(expected<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		throw std::runtime_error("read() from signalfd is not implemented");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t sequence) override, ([=] {
		std::cout << "posix: Fix signalfd::poll()" << std::endl;
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createSignalFile() {
	auto file = smarter::make_shared<OpenFile>();
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

