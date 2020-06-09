
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <helix/ipc.hpp>
#include "fs.hpp"
#include "eventfd.hpp"
#include "process.hpp"
#include "vfs.hpp"

namespace eventfd {

namespace {

struct OpenFile : File {
	OpenFile(unsigned int initval, bool nonBlock)
	: File{StructName::get("eventfd")}, _currentSeq{1}, _readableSeq{0},
		_writeableSeq{0}, _counter{initval}, _nonBlock{nonBlock} { }

	~OpenFile() {
	}

	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		if (max_length < 8)
			co_return Error::illegalArguments;

		while (1) {
			if (_counter) {
				memcpy(data, &_counter, 8);
				_counter = 0;
				_writeableSeq = ++_currentSeq;
				_doorbell.ring();
				co_return 8;
			}

			if (_nonBlock)
				co_return Error::wouldBlock;
			else
				co_await _doorbell.async_wait();
		}
	}

	virtual FutureMaybe<void> writeAll(Process *process, const void *data, size_t length) override {
		assert(length >= 8); // TODO: return Error::illegalArguments to user instead

		uint64_t num;
		memcpy(&num, data, 8);

		assert(num != 0xFFFFFFFFFFFFFFFF); // TODO: return Error::wouldBlock to user instead

		if (num && num + _counter <= _counter) {
			if (_nonBlock)
				assert(!"return Error::wouldBlock from eventfd::OpenFile::writeAll");
			else
				co_await _doorbell.async_wait(); // wait for read
		}

		_counter += num;

		_readableSeq = ++_currentSeq;
		_doorbell.ring();
	}

	expected<PollResult> poll(Process *, uint64_t sequence,
			async::cancellation_token cancellation) override {

		assert(sequence <= _currentSeq);
		while (_currentSeq == sequence &&
				!cancellation.is_cancellation_requested())
			co_await _doorbell.async_wait(cancellation);

		int edges = 0;
		if (_readableSeq > sequence)
			edges |= EPOLLIN;
		if (_writeableSeq > sequence)
			edges |= EPOLLOUT;

		int events = 0;
		if (_counter > 0)
			events |= EPOLLIN;
		if (_counter < 0xFFFFFFFFFFFFFFFF)
			events |= EPOLLOUT;

		co_return PollResult(_currentSeq, edges, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	async::doorbell _doorbell;

	uint64_t _currentSeq;
	uint64_t _readableSeq;
	uint64_t _writeableSeq;

	uint64_t _counter;
	bool _nonBlock;
};

}

smarter::shared_ptr<File, FileHandle> createFile(unsigned int initval, bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(initval, nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

}
