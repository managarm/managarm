
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "fifo.hpp"

namespace fifo {

namespace {

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	size_t offset = 0;
};

struct Channel {
	Channel()
	: currentSeq{1}, inSeq{0}, writerCount{0} { }

	// Status management for poll().
	async::doorbell statusBell;
	uint64_t currentSeq;
	uint64_t inSeq;
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
		protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
				&File::fileOperations);
	}

	ReaderFile()
	: File{StructName::get("fifo.read")} { }

	void connect(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
	}

	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		std::cout << "posix: Read from pipe " << this << std::endl;

		while(_channel->packetQueue.empty() && _channel->writerCount)
			COFIBER_AWAIT _channel->statusBell.async_wait();
		
		if(_channel->packetQueue.empty()) {
			assert(!_channel->writerCount);
			COFIBER_RETURN(0);
		}

		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_channel->packetQueue.front();
		assert(!packet->offset);
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_channel->packetQueue.pop_front();
		COFIBER_RETURN(size);
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t) override, ([=] {
		std::cout << "posix: Fix fifo readerFile::poll()" << std::endl;
	}))

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
		protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
				&File::fileOperations);
	}

	WriterFile()
	: File{StructName::get("fifo.write")} { }

	void connect(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
		_channel->writerCount++;
	}

	void handleClose() override {
		std::cout << "\e[35mposix: Cancel passthrough on fifo WriterFile::handleClose()\e[39m"
				<< std::endl;
		if(_channel->writerCount-- == 1) {
			_channel->currentSeq++;
			_channel->statusBell.ring();
		}
		_channel = nullptr;
	}
	
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t) override, ([=] {
		std::cout << "posix: Fix fifo WriterFile::poll()" << std::endl;
	}))

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
	r_file->connect(channel);
	w_file->connect(channel);
	ReaderFile::serve(r_file);
	WriterFile::serve(w_file);
	return {File::constructHandle(std::move(r_file)),
			File::constructHandle(std::move(w_file))};
}

} // namespace fifo

