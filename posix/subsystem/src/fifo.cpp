
#include <string.h>
#include <sys/epoll.h>
#include <iostream>
#include <deque>
#include <map>
#include <numeric>

#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/ipc.hpp>
#include "fifo.hpp"
#include "fs.bragi.hpp"

#include <sys/ioctl.h>

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
	: writerCount{0}, readerCount{0} { }

	// Status management for poll().
	async::recurring_event statusBell;
	// Start at currentSeq = 1 since the pipe is always writable.
	uint64_t currentSeq = 1;
	uint64_t noWriterSeq = 0;
	uint64_t noReaderSeq = 0;
	uint64_t inSeq = 0;
	int writerCount;
	int readerCount;

	async::recurring_event readerPresent;
	async::recurring_event writerPresent;

	// The actual queue of this pipe.
	std::deque<Packet> packetQueue;
};

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	OpenFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		bool isReader, bool isWriter, bool nonBlock = false)
	: File{StructName::get("fifo"), mount, link, File::defaultPipeLikeSeek},
		isReader_{isReader}, isWriter_{isWriter}, nonBlock_{nonBlock} { }

	void connectChannel(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
		if (isReader_)
			_channel->readerCount++;
		if (isWriter_)
			_channel->writerCount++;
	}

	void handleClose() override {
		std::cout << "\e[35mposix: Cancel passthrough on fifo OpenFile::handleClose()\e[39m"
				<< std::endl;
		if (isReader_) {
			if (_channel->readerCount-- == 1) {
				_channel->noReaderSeq = ++_channel->currentSeq;
				_channel->statusBell.raise();
			}
		}
		if (isWriter_) {
			if(_channel->writerCount-- == 1) {
				_channel->noWriterSeq = ++_channel->currentSeq;
				_channel->statusBell.raise();
			}
		}
		_channel = nullptr;
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t maxLength) override {
		if(logFifos)
			std::cout << "posix: Read from pipe " << this << std::endl;
		if (!isReader_)
			co_return Error::insufficientPermissions;
		if(!maxLength)
			co_return 0;

		while(_channel->packetQueue.empty() && _channel->writerCount) {
			if(nonBlock_) {
				if(logFifos)
					std::cout << "posix: FIFO pipe would block" << std::endl;
				co_return Error::wouldBlock;
			}
			co_await _channel->statusBell.async_wait();
		}

		if(_channel->packetQueue.empty()) {
			assert(!_channel->writerCount);
			co_return 0;
		}

		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_channel->packetQueue.front();
		size_t chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
		assert(chunk); // Otherwise we return above since !maxLength.
		memcpy(data, packet->buffer.data() + packet->offset, chunk);
		packet->offset += chunk;
		if(packet->offset == packet->buffer.size())
			_channel->packetQueue.pop_front();
		co_return chunk;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t maxLength) override {
		if (!isWriter_)
			co_return Error::insufficientPermissions;

		Packet packet;
		packet.buffer.resize(maxLength);
		memcpy(packet.buffer.data(), data, maxLength);
		packet.offset = 0;

		_channel->packetQueue.push_back(std::move(packet));
		_channel->inSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
		co_return maxLength;
	}


	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t pastSeq, int mask,
			async::cancellation_token cancellation) override {
		(void)mask; // TODO: utilize mask.
		// TODO: Return Error::fileClosed as appropriate.
		assert(pastSeq <= _channel->currentSeq);
		while(_channel && !cancellation.is_cancellation_requested()
				&& pastSeq == _channel->currentSeq)
			co_await _channel->statusBell.async_wait(cancellation);

		if(!_channel) {
			co_return Error::fileClosed;
		}

		if(cancellation.is_cancellation_requested())
			std::cout << "\e[33mposix: fifo::pollWait() cancellation is untested\e[39m" << std::endl;

		int edges = 0;
		if (isReader_) {
			if(_channel->noWriterSeq > pastSeq)
				edges |= EPOLLHUP;
			if(_channel->inSeq > pastSeq)
				edges |= EPOLLIN;
		}
		if (isWriter_) {
			edges |= EPOLLOUT;
			if(_channel->noReaderSeq > pastSeq)
				edges |= EPOLLERR;
		}

		co_return PollWaitResult(_channel->currentSeq, edges);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		int events = 0;
		if (isReader_) {
			if(!_channel->writerCount)
				events |= EPOLLHUP;
			if(!_channel->packetQueue.empty())
				events |= EPOLLIN;
		}
		if (isWriter_) {
			events |= EPOLLOUT;
			if(!_channel->readerCount)
				events |= EPOLLERR;
		}

		co_return PollStatusResult(_channel->currentSeq, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override {
		std::cout << "posix: setFileFlags on fifo \e[1;34m" << structName() << "\e[0m only supports O_NONBLOCK" << std::endl;
		if(flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on socket \e[1;34m" << structName() << "\e[0m called with unknown flags" << std::endl;
			co_return;
		}
		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		int flags;
		if (isReader_ && isWriter_) {
			flags = O_RDWR;
		} else if(isReader_) {
			flags = O_RDONLY;
		} else {
			assert(isWriter_);
			flags = O_WRONLY;
		}

		if(nonBlock_)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	async::result<void>
	ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		managarm::fs::GenericIoctlReply resp;

		if(id == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
			assert(req);

			switch(req->command()) {
				case FIONREAD: {
					size_t count = 0;
					if (isReader_)
						count = std::accumulate(_channel->packetQueue.cbegin(), _channel->packetQueue.cend(), 0,
							[](size_t sum, const Packet &p) {
								return sum + (p.buffer.size() - p.offset);
							}
						);

					resp.set_fionread_count(count);
					resp.set_error(managarm::fs::Errors::SUCCESS);

					break;
				}
				default: {
					std::cout << "Invalid ioctl for fifo.read" << std::endl;
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
		}else{
			std::cout << "\e[31m" "fifo: Unknown ioctl() message with ID "
					<< id << "\e[39m" << std::endl;

			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;

	bool isReader_;
	bool isWriter_;
	bool nonBlock_;
};

} // anonymous namespace

// This maps FsNodes to Channels for named pipes (FIFOs)
std::map<FsNode *, std::shared_ptr<Channel>> globalChannelMap;

void createNamedChannel(FsNode *node) {
	assert(globalChannelMap.find(node) == globalChannelMap.end());
	globalChannelMap[node] = std::make_shared<Channel>();
}

void unlinkNamedChannel(FsNode *node) {
	assert(globalChannelMap.find(node) != globalChannelMap.end());
	globalChannelMap.erase(node);
}

async::result<smarter::shared_ptr<File, FileHandle>>
openNamedChannel(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, FsNode *node, SemanticFlags flags) {
	if (globalChannelMap.find(node) == globalChannelMap.end())
		co_return nullptr;

	auto channel = globalChannelMap.at(node);

	if (flags & semanticRead) {
		assert(!(flags & semanticWrite));

		auto r_file = smarter::make_shared<OpenFile>(mount, link, true, false);
		r_file->setupWeakFile(r_file);
		r_file->connectChannel(channel);

		channel->readerPresent.raise();
		if (!channel->writerCount && !(flags & semanticNonBlock))
			co_await channel->writerPresent.async_wait();

		OpenFile::serve(r_file);

		co_return File::constructHandle(std::move(r_file));
	} else {
		assert(flags & semanticWrite);
		assert(!(flags & semanticRead));

		auto w_file = smarter::make_shared<OpenFile>(mount, link, false, true);
		w_file->setupWeakFile(w_file);
		w_file->connectChannel(channel);

		channel->writerPresent.raise();
		if (!channel->readerCount && !(flags & semanticNonBlock))
			co_await channel->readerPresent.async_wait();

		OpenFile::serve(w_file);

		co_return File::constructHandle(std::move(w_file));
	}
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair(bool nonBlock) {
	auto link = SpecialLink::makeSpecialLink(VfsType::fifo, 0777);
	auto channel = std::make_shared<Channel>();
	auto r_file = smarter::make_shared<OpenFile>(nullptr, link, true, false, nonBlock);
	auto w_file = smarter::make_shared<OpenFile>(nullptr, link, false, true, nonBlock);
	r_file->setupWeakFile(r_file);
	w_file->setupWeakFile(w_file);
	r_file->connectChannel(channel);
	w_file->connectChannel(channel);
	OpenFile::serve(r_file);
	OpenFile::serve(w_file);
	return {File::constructHandle(std::move(r_file)),
			File::constructHandle(std::move(w_file))};
}

} // namespace fifo

