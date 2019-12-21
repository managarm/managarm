
#include <string.h>
#include <sys/epoll.h>
#include <iostream>
#include <deque>

#include <async/doorbell.hpp>
#include <helix/ipc.hpp>
#include "fifo.hpp"

#include <experimental/coroutine>

namespace fifo {

namespace {

constexpr bool logFifos = false;

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	size_t offset = 0;
};

struct Channel {
	Channel()
	: writerCount{0} { }

	// Status management for poll().
	async::doorbell statusBell;
	uint64_t currentSeq = 0;
	uint64_t hupSeq = 0;
	uint64_t inSeq = 1;
	int writerCount;

	// The actual queue of this pipe.
	std::deque<Packet> packetQueue;
};

struct ReaderFile : File {
public:
	static void serve(smarter::shared_ptr<ReaderFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	ReaderFile()
	: File{StructName::get("fifo.read"), File::defaultPipeLikeSeek} { }

	void connectChannel(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
	}

	expected<size_t> readSome(Process *, void *data, size_t max_length) override {
		if(logFifos)
			std::cout << "posix: Read from pipe " << this << std::endl;

		while(_channel->packetQueue.empty() && _channel->writerCount)
			co_await _channel->statusBell.async_wait();

		if(_channel->packetQueue.empty()) {
			assert(!_channel->writerCount);
			co_return 0;
		}

		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_channel->packetQueue.front();
		assert(!packet->offset);
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_channel->packetQueue.pop_front();
		co_return size;
	}

	expected<PollResult> poll(Process *, uint64_t past_seq,
			async::cancellation_token cancellation) override {
		// TODO: Return Error::fileClosed as appropriate.
		assert(past_seq <= _channel->currentSeq);
		while(past_seq == _channel->currentSeq
				&& !cancellation.is_cancellation_requested())
			co_await _channel->statusBell.async_wait(cancellation);

		if(cancellation.is_cancellation_requested())
			std::cout << "\e[33mposix: fifo::poll() cancellation is untested\e[39m" << std::endl;

		int edges = 0;
		if(_channel->hupSeq > past_seq)
			edges |= EPOLLHUP;
		if(_channel->inSeq > past_seq)
			edges |= EPOLLIN;

		int events = 0;
		if(!_channel->writerCount) {
			events |= EPOLLHUP;
		}
		if(!_channel->packetQueue.empty())
			events |= EPOLLIN;

		co_return PollResult(_channel->currentSeq, edges, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;
};

struct WriterFile : File {
public:
	static void serve(smarter::shared_ptr<WriterFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	WriterFile()
	: File{StructName::get("fifo.write"), File::defaultPipeLikeSeek} { }

	void connectChannel(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
		_channel->writerCount++;
	}

	void handleClose() override {
		std::cout << "\e[35mposix: Cancel passthrough on fifo WriterFile::handleClose()\e[39m"
				<< std::endl;
		if(_channel->writerCount-- == 1) {
			_channel->hupSeq = ++_channel->currentSeq;
			_channel->statusBell.ring();
		}
		_channel = nullptr;
	}

	FutureMaybe<void> writeAll(Process *process, const void *data, size_t max_length) override {

		Packet packet;
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.offset = 0;

		_channel->packetQueue.push_back(std::move(packet));
		_channel->inSeq = ++_channel->currentSeq;
		_channel->statusBell.ring();

		co_return;
	}

	expected<PollResult> poll(Process *, uint64_t, async::cancellation_token) override {
		std::cout << "posix: Fix fifo WriterFile::poll()" << std::endl;
		co_await std::experimental::suspend_always{};
		__builtin_unreachable();
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;
};

} // anonymous namespace

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair() {
	auto channel = std::make_shared<Channel>();
	auto r_file = smarter::make_shared<ReaderFile>();
	auto w_file = smarter::make_shared<WriterFile>();
	r_file->setupWeakFile(r_file);
	w_file->setupWeakFile(w_file);
	r_file->connectChannel(channel);
	w_file->connectChannel(channel);
	ReaderFile::serve(r_file);
	WriterFile::serve(w_file);
	return {File::constructHandle(std::move(r_file)),
			File::constructHandle(std::move(w_file))};
}

} // namespace fifo

