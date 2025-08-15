#pragma once

#include <async/recurring-event.hpp>

#include "file.hpp"
#include "process.hpp"

namespace pidfd {

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file);

	OpenFile(std::weak_ptr<ThreadGroup> proc, bool nonBlock);

	async::result<std::expected<size_t, Error>>
	readSome(Process *process, void *data, size_t maxLength, async::cancellation_token ct) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *process, uint64_t inSeq, int pollMask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *process) override;

	async::result<std::string> getFdInfo() override;

	void handleClose() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return passthrough_;
	}

	int pid() const;

	bool nonBlock() const {
		return nonBlock_;
	}
private:
	helix::UniqueLane passthrough_;
	async::cancellation_event cancelServe_;
	bool nonBlock_;
	std::weak_ptr<ThreadGroup> process_;
};

} // namespace pidfd

smarter::shared_ptr<File, FileHandle> createPidfdFile(std::weak_ptr<ThreadGroup> proc, bool nonBlock);
