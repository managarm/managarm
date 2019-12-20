
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "fs.hpp"
#include "inotify.hpp"
#include "process.hpp"
#include "vfs.hpp"

namespace inotify {

namespace {

struct OpenFile : File {
public:
	struct Packet {
		// TODO: Keep track of the event mask and file name.
	};

	struct Watch final : FsObserver {
		Watch(OpenFile *file_)
		: file{file_} { }

		void observeNotification() override {
			file->_queue.push_back(Packet{});
			file->_inSeq = ++file->_currentSeq;
			file->_statusBell.ring();
		}

		OpenFile *file;
	};

	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	OpenFile()
	: File{StructName::get("inotify")} { }

	~OpenFile() {
		// TODO: Properly keep track of watches.
		std::cout << "\e[31m" "posix: Destruction of inotify leaks watches" "\e[39m" << std::endl;
	}

	expected<size_t> readSome(Process *, void *data, size_t max_length) override {
		throw std::runtime_error("read() from inotify is not implemented");
	}

	expected<PollResult> poll(Process *, uint64_t sequence, async::cancellation_token cancellation) override {
		// TODO: Return Error::fileClosed as appropriate.

		assert(sequence <= _currentSeq);
		while(sequence == _currentSeq
				&& !cancellation.is_cancellation_requested())
			co_await _statusBell.async_wait(cancellation);

		int edges = 0;
		if(_inSeq > sequence)
			edges |= EPOLLIN;

		int events = 0;
		if(!_queue.empty())
			events |= EPOLLIN;

		co_return PollResult(_currentSeq, edges, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	int addWatch(std::shared_ptr<FsNode> node) {
		// TODO: Coalesce watch descriptors for the same inode.
		auto watch = std::make_shared<Watch>(this);
		node->addObserver(watch);
		return _nextDescriptor++;
	}

private:
	helix::UniqueLane _passthrough;
	std::deque<Packet> _queue;

	// TODO: Use a proper ID allocator to allocate watch descriptor IDs.
	int _nextDescriptor = 1;

	async::doorbell _statusBell;
	uint64_t _currentSeq = 1;
	uint64_t _inSeq = 0;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

int addWatch(File *base, std::shared_ptr<FsNode> node) {
	auto file = static_cast<OpenFile *>(base);
	return file->addWatch(std::move(node));
}

} // namespace inotify

