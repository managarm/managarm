
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <iostream>

#include <async/recurring-event.hpp>
#include <helix/ipc.hpp>
#include "process.hpp"
#include "signalfd.hpp"

namespace signal_fd {

void OpenFile::serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			smarter::shared_ptr<File>{file}, &File::fileOperations));
}

OpenFile::OpenFile(uint64_t mask, bool nonBlock)
: File{StructName::get("signalfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)}, _mask{mask}, _nonBlock{nonBlock} { }

async::result<frg::expected<Error, size_t>>
OpenFile::readSome(Process *process, void *data, size_t maxLength) {
	if(maxLength < sizeof(struct signalfd_siginfo))
		co_return Error::illegalArguments;

	auto active = co_await process->signalContext()->fetchSignal(_mask, _nonBlock);
	if(!active)
		co_return Error::wouldBlock;

	struct signalfd_siginfo si = {};
	si.ssi_signo = active->signalNumber;

	memcpy(data, &si, sizeof(struct signalfd_siginfo));
	co_return sizeof(struct signalfd_siginfo);
}

async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollWait(Process *process, uint64_t inSeq, int pollMask,
		async::cancellation_token cancellation) {
	(void)pollMask; // TODO: utilize mask.
	auto result = co_await process->signalContext()->pollSignal(inSeq,
			_mask, cancellation);
	co_return PollWaitResult{std::get<0>(result),
			(std::get<1>(result) & _mask) ? EPOLLIN : 0};
}

async::result<frg::expected<Error, PollStatusResult>>
OpenFile::pollStatus(Process *process) {
	auto result = process->signalContext()->checkSignal();
	co_return PollStatusResult{std::get<0>(result),
			(std::get<1>(result) & _mask) ? EPOLLIN : 0};
}

} // namespace signal_fd

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask, bool nonBlock) {
	auto file = smarter::make_shared<signal_fd::OpenFile>(mask, nonBlock);
	file->setupWeakFile(file);
	signal_fd::OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

