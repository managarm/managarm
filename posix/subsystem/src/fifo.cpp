
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "fifo.hpp"

namespace fifo {

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
	: File{StructName::get("fifo")} { }

	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *, size_t) override, ([=] {
		throw std::runtime_error("read() from fifo is not implemented");
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t) override, ([=] {
		std::cout << "posix: Fix fifo::poll()" << std::endl;
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

} // anonymous namespace

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair() {
	auto r_file = smarter::make_shared<OpenFile>();
	auto w_file = smarter::make_shared<OpenFile>();
	OpenFile::serve(r_file);
	OpenFile::serve(w_file);
	return {File::constructHandle(std::move(r_file)),
			File::constructHandle(std::move(w_file))};
}

} // namespace fifo

