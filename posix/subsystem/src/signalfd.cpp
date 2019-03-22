
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "process.hpp"
#include "signalfd.hpp"

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

	OpenFile(uint64_t mask)
	: File{StructName::get("signalfd")}, _mask{mask} { }

	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		throw std::runtime_error("read() from signalfd is not implemented");
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(Process *process, uint64_t in_seq,
			async::cancellation_token cancellation) override, ([=] {
		std::cout << "posix: signalfd poll() with mask " << _mask << std::endl;
		auto sequence = COFIBER_AWAIT process->signalContext()->pollSignal(in_seq, _mask,
				cancellation);
		std::cout << "posix: signalfd poll() returns" << std::endl;
		COFIBER_RETURN(PollResult(sequence, EPOLLIN, EPOLLIN));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	uint64_t _mask;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask) {
	auto file = smarter::make_shared<OpenFile>(mask);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

