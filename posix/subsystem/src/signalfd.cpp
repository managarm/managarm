
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <iostream>

#include <async/doorbell.hpp>
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

	expected<size_t> readSome(Process *process, void *data, size_t max_length) override {
		// TODO: Return an error otherwise.
		assert(max_length >= sizeof(struct signalfd_siginfo));

		auto active = process->signalContext()->fetchSignal(_mask);
		assert(active); // TODO: Implement blocking for signals.

		struct signalfd_siginfo si = {};
		si.ssi_signo = active->signalNumber;

		memcpy(data, &si, sizeof(struct signalfd_siginfo));
		co_return sizeof(struct signalfd_siginfo);
	}
	
	expected<PollResult> poll(Process *process, uint64_t in_seq,
			async::cancellation_token cancellation) override {
		auto result = co_await process->signalContext()->pollSignal(in_seq,
				_mask, cancellation);
		co_return PollResult(std::get<0>(result), EPOLLIN, EPOLLIN);
	}

	expected<PollResult> checkStatus(Process *process) override {
		auto result = process->signalContext()->checkSignal(_mask);
		co_return PollResult(std::get<0>(result),
					(std::get<1>(result) & _mask) ? EPOLLIN : 0,
					(std::get<2>(result) & _mask) ? EPOLLIN : 0);
	}

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

