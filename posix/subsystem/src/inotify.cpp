
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "inotify.hpp"

namespace inotify {

namespace {

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	OpenFile()
	: File{StructName::get("inotify")} { }

	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		throw std::runtime_error("read() from inotify is not implemented");
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(Process *, uint64_t sequence,
			async::cancellation_token) override, ([=] {
		std::cout << "posix: Fix inotify::poll()" << std::endl;
		COFIBER_AWAIT cofiber::suspend_always{};
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

} // namespace inotify

