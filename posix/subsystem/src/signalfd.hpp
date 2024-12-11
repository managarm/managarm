#pragma once

#include "file.hpp"

namespace signal_fd {

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file);

	OpenFile(uint64_t mask, bool nonBlock);

	async::result<frg::expected<Error, size_t>>
	readSome(Process *process, void *data, size_t maxLength) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *process, uint64_t inSeq, int pollMask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *process) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	uint64_t &mask() {
		return _mask;
	}

private:
	helix::UniqueLane _passthrough;
	uint64_t _mask;
	bool _nonBlock;
};

} // namespace signal_fd

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask, bool nonBlock);
