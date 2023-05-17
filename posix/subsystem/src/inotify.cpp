#include <async/cancellation.hpp>
#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/ipc.hpp>
#include <iostream>
#include <print>
#include <protocols/fs/common.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>

#include "fs.hpp"
#include "inotify.hpp"
#include "process.hpp"

namespace inotify {

namespace {

constexpr int supportedFlags = IN_DELETE | IN_CREATE | IN_ISDIR | IN_DELETE_SELF | IN_MODIFY | IN_ACCESS | IN_CLOSE;
constexpr int alwaysReturnedFlags = IN_IGNORED | IN_ISDIR | IN_Q_OVERFLOW | IN_UNMOUNT;

struct OpenFile : File {
public:
	struct Packet {
		int descriptor;
		uint32_t events;
		std::string name;
		uint32_t cookie;
	};

	struct Watch final : FsObserver {
		Watch(smarter::weak_ptr<File> file_, int descriptor, uint32_t mask, std::weak_ptr<FsNode> node)
		: file{file_}, descriptor{descriptor}, mask{mask}, node{node} { }

		void observeNotification(uint32_t events,
				const std::string &name, uint32_t cookie, bool isDir) override {
			uint32_t inotifyEvents = 0;
			if(events & FsObserver::deleteEvent)
				inotifyEvents |= IN_DELETE;
			if(events & FsObserver::deleteSelfEvent)
				inotifyEvents |= IN_DELETE_SELF;
			if(events & FsObserver::createEvent)
				inotifyEvents |= IN_CREATE;
			if(events & FsObserver::modifyEvent)
				inotifyEvents |= IN_MODIFY;
			if(events & FsObserver::accessEvent)
				inotifyEvents |= IN_ACCESS;
			if(events & FsObserver::closeWriteEvent)
				inotifyEvents |= IN_CLOSE_WRITE;
			if(events & FsObserver::closeNoWriteEvent)
				inotifyEvents |= IN_CLOSE_NOWRITE;
			if(events & FsObserver::ignoredEvent)
				inotifyEvents |= IN_IGNORED;

			auto f = smarter::static_pointer_cast<OpenFile>(file.lock());
			if(!f)
				return;

			auto preexists = std::ranges::find_if(f->_queue, [&name, inotifyEvents](auto a) {
				return a.name == name && (a.events & inotifyEvents) == inotifyEvents;
			}) != f->_queue.end();
			if(preexists)
				return;

			if(isDir)
				inotifyEvents |= IN_ISDIR;

			if(!(inotifyEvents & (mask | alwaysReturnedFlags)))
				return;

			f->_queue.push_back(Packet{descriptor, inotifyEvents & (mask | alwaysReturnedFlags), name, cookie});
			f->_inSeq = ++f->_currentSeq;
			f->_statusBell.raise();
		}

		smarter::weak_ptr<File> file;
		int descriptor;
		uint32_t mask;
		std::weak_ptr<FsNode> node;
	};

	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
			smarter::shared_ptr<File>{file}, &File::fileOperations, file->cancelServe_));
	}

	void handleClose() override {
		cancelServe_.cancel();
		_passthrough = {};

		for(auto [_, watch] : watches_) {
			auto node = watch->node.lock();

			if(node)
				node->removeObserver(watch.get());
		}
	}

	OpenFile(bool nonBlock = false)
	: File{FileKind::inotify,  StructName::get("inotify"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)}, nonBlock_{nonBlock} { }

	~OpenFile() override { }

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) override {
		if (_queue.empty() && nonBlock_)
			co_return std::unexpected{Error::wouldBlock};

		while(_queue.empty()) {
			if (!co_await _statusBell.async_wait(ce))
				co_return std::unexpected{Error::interrupted};
		}

		size_t written = 0;

		while(written < maxLength) {
			size_t packetSize = sizeof(inotify_event) +
				(_queue.front().name.empty() ? 0 : _queue.front().name.size() + 1);

			if(written + packetSize > maxLength)
				break;

			Packet packet = std::move(_queue.front());
			_queue.pop_front();

			inotify_event e;
			memset(&e, 0, sizeof(inotify_event));
			e.wd = packet.descriptor;
			e.mask = packet.events;
			e.cookie = packet.cookie;
			if(!packet.name.empty()) {
				e.len = packet.name.size() + 1;
				memcpy(reinterpret_cast<char *>(data) + written + sizeof(inotify_event),
					packet.name.c_str(), packet.name.size() + 1);
			}

			memcpy(reinterpret_cast<char *>(data) + written, &e, sizeof(inotify_event));

			written += packetSize;

			if(_queue.empty())
				break;
		}

		if(written)
			co_return written;
		else
			co_return std::unexpected{Error::illegalArguments};
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
	async::cancellation_token cancellation) override {
		(void)mask; // TODO: utilize mask.
		// TODO: Return Error::fileClosed as appropriate.

		assert(sequence <= _currentSeq);
		while(sequence == _currentSeq
				&& !cancellation.is_cancellation_requested())
			co_await _statusBell.async_wait(cancellation);

		int edges = 0;
		if(_inSeq > sequence)
			edges |= EPOLLIN;

		co_return PollWaitResult(_currentSeq, edges);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		int events = 0;
		if(!_queue.empty())
			events |= EPOLLIN;

		co_return PollStatusResult(_currentSeq, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	int addWatch(std::shared_ptr<FsNode> node, uint32_t mask) {
		if(mask & ~supportedFlags)
			std::println("posix: inotify mask {:#x} is partially ignored", mask);

		for(const auto &[desc, watch] : watches_)
			if(watch->file.lock().get() == this)
				return desc;

		auto descriptor = descriptorAllocator_.allocate();
		auto watch = std::make_shared<Watch>(weakFile(), descriptor, mask, node);
		node->addObserver(watch);
		watches_.insert({descriptor, std::move(watch)});
		return descriptor;
	}

	bool removeWatch(int wd) {
		if(!watches_.contains(wd))
			return false;

		auto watch = watches_.at(wd);
		auto node = watch->node.lock();

		if(node) {
			node->removeObserver(watch.get());
			watch->observeNotification(FsObserver::ignoredEvent, {}, 0, false);
		}

		watches_.erase(wd);
		descriptorAllocator_.free(wd);

		return true;
	}

	async::result<void>
	ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		managarm::fs::GenericIoctlReply resp;

		if(id == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
			msg.reset();
			assert(req);

			switch(req->command()) {
				case FIONREAD: {
					resp.set_error(managarm::fs::Errors::SUCCESS);

					if(_queue.empty()) {
						resp.set_fionread_count(0);
					} else {
						auto packet = &_queue.front();
						auto size = sizeof(Packet) + packet->name.size() + 1;
						resp.set_fionread_count(size);
					}
					break;
				}
				default: {
					std::cout << "Invalid ioctl for inotify" << std::endl;
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					break;
				}
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event cancelServe_;
	std::deque<Packet> _queue;

	id_allocator<int> descriptorAllocator_{1};
	std::unordered_map<int, std::shared_ptr<Watch>> watches_;

	async::recurring_event _statusBell;
	uint64_t _currentSeq = 1;
	uint64_t _inSeq = 0;

	bool nonBlock_;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createFile(bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

int addWatch(File *base, std::shared_ptr<FsNode> node, uint32_t mask) {
	auto file = static_cast<OpenFile *>(base);
	return file->addWatch(std::move(node), mask);
}

bool removeWatch(File *base, int wd) {
	auto file = static_cast<OpenFile *>(base);
	return file->removeWatch(wd);
}

} // namespace inotify
