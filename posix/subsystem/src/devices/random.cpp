#include <string.h>
#include <sys/random.h>

#include "../common.hpp"
#include "random.hpp"

#include <experimental/coroutine>

namespace {

struct RandomFile final : File {
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
	static void serve(smarter::shared_ptr<RandomFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	RandomFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("random-file"), std::move(mount), std::move(link)} { }
};

struct RandomDevice final : UnixDevice {
	RandomDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 8});
	}
	
	std::string nodePath() override {
		return "random";
	}
	
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite))
			co_return Error::illegalArguments;

		auto file = smarter::make_shared<RandomFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		RandomFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createRandomDevice() {
	return std::make_shared<RandomDevice>();
}
