
#include <async/cancellation.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <print>

#include <async/recurring-event.hpp>
#include <helix/ipc.hpp>
#include "eventfd.hpp"
#include "process.hpp"

namespace eventfd {

namespace {

struct OpenFile : File {
	OpenFile(unsigned int initval, bool nonBlock, bool semaphore)
	: File{FileKind::unknown,  StructName::get("eventfd")}, _currentSeq{1}, _readableSeq{0},
		_writeableSeq{0}, _counter{initval}, _nonBlock{nonBlock}, _semaphore{semaphore} { }

	~OpenFile() override {
	}

	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations, file->cancelServe_));
	}

	void handleClose() override {
		cancelServe_.cancel();
		_passthrough = {};
	}

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override {
		if (max_length < 8)
			co_return std::unexpected{Error::illegalArguments};

		while (1) {
			if (_counter) {
				if(_semaphore) {
					auto ptr = reinterpret_cast<uint64_t *>(data);
					*ptr = 1;
					_counter--;
				} else {
					memcpy(data, &_counter, 8);
					_counter = 0;
				}
				_writeableSeq = ++_currentSeq;
				_doorbell.raise();
				co_return size_t{8};
			}

			if (_nonBlock) {
				co_return std::unexpected{Error::wouldBlock};
			} else {
				if (!co_await _doorbell.async_wait(ce))
					co_return std::unexpected{Error::interrupted};
			}
		}
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override {
		if(length != 8)
			co_return Error::illegalArguments;

		uint64_t num;
		memcpy(&num, data, 8);

		if(num == 0xFFFFFFFFFFFFFFFF)
			co_return Error::illegalArguments;

		if (num && num + _counter <= _counter) {
			if (_nonBlock)
				co_return Error::wouldBlock;
			else
				co_await _doorbell.async_wait(); // wait for read
		}

		_counter += num;

		_readableSeq = ++_currentSeq;
		_doorbell.raise();
		co_return length;
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask; // TODO: utilize mask.

		assert(sequence <= _currentSeq);
		while (_currentSeq == sequence &&
				!cancellation.is_cancellation_requested())
			co_await _doorbell.async_wait(cancellation);

		int edges = 0;
		if (_readableSeq > sequence)
			edges |= EPOLLIN;
		if (_writeableSeq > sequence)
			edges |= EPOLLOUT;

		co_return PollWaitResult(_currentSeq, edges);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		int events = 0;
		if (_counter > 0)
			events |= EPOLLIN;
		if (_counter < 0xFFFFFFFFFFFFFFFF)
			events |= EPOLLOUT;

		co_return PollStatusResult(_currentSeq, events);
	}

	async::result<void> setFileFlags(int flags) override {
		// ignore O_{RDWR,WRONLY,RDONLY}
		flags &= ~(O_RDWR | O_WRONLY | O_RDONLY);

		if (flags & ~O_NONBLOCK) {
			std::println("posix: setFileFlags on eventfd called with unknown flags {:#x}", flags & ~O_NONBLOCK);
			co_return;
		}

		_nonBlock = (flags & O_NONBLOCK);
		co_return;
	}

	async::result<int> getFileFlags() override {
		int flags = O_RDWR;
		if(_nonBlock)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	async::recurring_event _doorbell;
	async::cancellation_event cancelServe_;

	uint64_t _currentSeq;
	uint64_t _readableSeq;
	uint64_t _writeableSeq;

	uint64_t _counter;
	bool _nonBlock;
	bool _semaphore;
};

}

smarter::shared_ptr<File, FileHandle> createFile(unsigned int initval, bool nonBlock, bool semaphore) {
	auto file = smarter::make_shared<OpenFile>(initval, nonBlock, semaphore);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

}
