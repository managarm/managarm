
#include <async/cancellation.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <iostream>

#include <async/recurring-event.hpp>
#include <helix/ipc.hpp>
#include "process.hpp"
#include "protocols/fs/common.hpp"
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

	OpenFile(uint64_t mask, bool nonBlock)
	: File{StructName::get("signalfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)}, _mask{mask}, _nonBlock{nonBlock} { }

	async::result<protocols::fs::ReadResult>
	readSome(Process *process, void *data, size_t maxLength, async::cancellation_token ce) override {
		if(maxLength < sizeof(struct signalfd_siginfo))
			co_return {protocols::fs::Error::illegalArguments, 0};

		async::cancellation_event ev;
		if (_nonBlock) {
			ev.cancel();
			ce = async::cancellation_token{ev};
		}
		auto active = co_await process->signalContext()->fetchSignal(_mask, ce);
		if(!active && _nonBlock)
			co_return {protocols::fs::Error::wouldBlock, 0};
		if (!active && !_nonBlock)
			co_return {protocols::fs::Error::interrupted, 0};

		struct signalfd_siginfo si = {};
		si.ssi_signo = active->signalNumber;

		memcpy(data, &si, sizeof(struct signalfd_siginfo));
		co_return {protocols::fs::Error::none,
				sizeof(struct signalfd_siginfo)};
	}
	
	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *process, uint64_t inSeq, int pollMask,
			async::cancellation_token cancellation) override {
		(void)pollMask; // TODO: utilize mask.
		auto result = co_await process->signalContext()->pollSignal(inSeq,
				_mask, cancellation);
		co_return PollWaitResult{std::get<0>(result),
				(std::get<1>(result) & _mask) ? EPOLLIN : 0};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *process) override {
		auto result = process->signalContext()->checkSignal();
		co_return PollStatusResult{std::get<0>(result),
				(std::get<1>(result) & _mask) ? EPOLLIN : 0};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	uint64_t _mask;
	bool _nonBlock;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask, bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(mask, nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

