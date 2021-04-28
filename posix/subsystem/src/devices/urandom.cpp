#include <string.h>
#include <sys/random.h>

#include "../common.hpp"
#include "urandom.hpp"

#include <experimental/coroutine>

namespace {

struct UrandomFile final : File {
private:
	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t length) override {
		auto p = reinterpret_cast<char *>(data);
		size_t n = 0;
		while(n < length) {
			size_t chunk;
			HEL_CHECK(helGetRandomBytes(p + n, length - n, &chunk));
			n+= chunk;
		}

		co_return n;
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *, size_t length) override {
		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t, VfsSeek) override {
		co_return 0;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<UrandomFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	UrandomFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("urandom-file"), std::move(mount), std::move(link)} { }
};

struct UrandomDevice final : UnixDevice {
	UrandomDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 9});
	}
	
	std::string nodePath() override {
		return "urandom";
	}
	
	FutureMaybe<SharedFilePtr> open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		assert(!(semantic_flags & ~(semanticRead | semanticWrite)));
		auto file = smarter::make_shared<UrandomFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		UrandomFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createUrandomDevice() {
	return std::make_shared<UrandomDevice>();
}
