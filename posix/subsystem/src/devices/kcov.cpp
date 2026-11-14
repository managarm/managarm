#include <coroutine>
#include <mutex>
#include <print>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <helix/dispatcher-pool.hpp>
#include <protocols/fs/common.hpp>

#include "../common.hpp"
#include "../process.hpp"
#include "kcov.hpp"

namespace {

constexpr uint32_t kcovRequestSetSize = 0xF1177200;
constexpr uint32_t kcovRequestStart = 0xF1177000;
constexpr uint32_t kcovRequestStop = 0xF1177100;

struct KcovFile final : FileWithDefaults {
private:
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override {
		_cancelServe.cancel();
		_passthrough = {};
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::mutex _mutex;
	helix::UniqueDescriptor _buffer;

	managarm::fs::Errors setSize(int entries) {
		if(entries <= 0)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;

		std::lock_guard lock{_mutex};
		if(_buffer)
			return managarm::fs::Errors::BUSY;

		HelHandle handle;
		auto error = helCreateKcovBuffer((static_cast<size_t>(entries) + 1) * sizeof(uint64_t), &handle);
		if(error == kHelErrIllegalArgs)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;
		if(error == kHelErrNoMemory)
			return managarm::fs::Errors::NO_MEMORY;
		HEL_CHECK(error);
		_buffer = helix::UniqueDescriptor{handle};
		return managarm::fs::Errors::SUCCESS;
	}

	managarm::fs::Errors start(Process *process, int mode) {
		if(mode != kHelKcovModePc && mode != kHelKcovModeCmp)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;

		std::lock_guard lock{_mutex};
		if(!_buffer)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;

		auto error = helEnableCoverage(process->threadDescriptor().getHandle(),
				_buffer.getHandle(), mode);
		if(error == kHelErrIllegalState)
			return managarm::fs::Errors::BUSY;
		if(error == kHelErrThreadTerminated)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;
		HEL_CHECK(error);
		return managarm::fs::Errors::SUCCESS;
	}

	managarm::fs::Errors stop(Process *process) {
		std::lock_guard lock{_mutex};
		if(!_buffer)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;

		auto error = helDisableCoverage(process->threadDescriptor().getHandle(),
				_buffer.getHandle());
		if(error == kHelErrIllegalState)
			return managarm::fs::Errors::ILLEGAL_ARGUMENT;
		HEL_CHECK(error);
		return managarm::fs::Errors::SUCCESS;
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, KcovFile *self) {
			// The requests operate on the calling thread.
			auto [extractCreds] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials()
			);
			HEL_CHECK(extractCreds.error());

			managarm::fs::GenericIoctlReply resp;
			auto maybeProcess = findProcessWithCredentials(extractCreds.credentials());
			if(!maybeProcess) {
				std::println("posix: kcov ioctl with unknown process credentials");
				resp.set_error(Error::badProcessCredentials
						| protocols::fs::toFsProtoError | protocols::fs::toFsError);
			}else if(req.command() == kcovRequestSetSize) {
				resp.set_error(self->setSize(req.kcov_arg()));
			}else if(req.command() == kcovRequestStart) {
				resp.set_error(self->start(maybeProcess->get(), req.kcov_arg()));
			}else if(req.command() == kcovRequestStop) {
				resp.set_error(self->stop(maybeProcess->get()));
			}else{
				std::println("posix: unexpected kcov ioctl request 0x{:x}", req.command());
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			co_return {};
		}
	};

public:
	static void serve(smarter::shared_ptr<KcovFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	KcovFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: FileWithDefaults{FileKind::unknown, StructName::get("kcov-file"),
			std::move(mount), std::move(link)} { }

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		std::lock_guard lock{_mutex};
		co_return _buffer.dup();
	}

	async::result<void> ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if(!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
};

struct KcovDevice final : UnixDevice {
	KcovDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 254}); // This minor is not used by Linux.
	}

	std::string nodePath() override {
		return "kcov";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite))
			co_return Error::illegalArguments;

		auto file = smarter::make_shared<KcovFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		KcovFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createKcovDevice() {
	return std::make_shared<KcovDevice>();
}
