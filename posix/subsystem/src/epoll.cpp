
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include "epoll.hpp"

namespace {

struct OpenFile : ProxyFile {
	// ------------------------------------------------------------------------
	// Internal API.
	// ------------------------------------------------------------------------
private:
	struct Item : boost::intrusive::list_base_hook<> {
		Item(OpenFile *epoll, File *file, int mask, uint64_t cookie)
		: epoll{epoll}, file{file}, eventMask{mask}, cookie{cookie},
				isPending{false} { }

		// Basic data of this item.
		OpenFile *epoll;
		File *file;
		int eventMask;
		uint64_t cookie;

		// True iff this item is in the pending queue.
		bool isPending;
	};

	static void _awaitPoll(Item *item, PollResult result) {
		auto self = item->epoll;

		// Note that items only become pending if there is an edge.
		// This is the correct behavior for edge-triggered items.
		// Level-triggered items stay pending until the event disappears.
		if(!item->isPending && (std::get<1>(result) & item->eventMask)
				&& (std::get<2>(result) & item->eventMask)) {
			// Note that we stop watching once an item becomes pending.
			// We do this as we have to poll() again anyway before we report the item.
			item->isPending = true;
			self->_pendingQueue.push_back(*item);
			self->_pendingBell.ring();
		}else{
			// Here, we assume that the lambda does not execute on the current stack.
			// TODO: Use some callback queueing mechanism to ensure this.
			auto poll = item->file->poll(std::get<0>(result));
			poll.then([item] (PollResult next_result) {
				_awaitPoll(item, next_result);
			});
		}
	}

public:
	~OpenFile() {
		assert(!"close() does not work correctly for epoll files");
	}

	void watchFile(File *file, int mask, uint64_t cookie) {
		// TODO: Fix the memory-leak.
		assert(_fileMap.find(file) == _fileMap.end());
		auto item = new Item{this, file, mask, cookie};

		auto poll = item->file->poll(0);
		poll.then([item] (PollResult result) {
			_awaitPoll(item, result);
		});

		_fileMap.insert({file, item});
	}

	COFIBER_ROUTINE(async::result<struct epoll_event>, waitForEvent(), ([=] {
		while(true) {
			while(_pendingQueue.empty())
				COFIBER_AWAIT _pendingBell.async_wait();

			// TODO: Stealing all elements might lead to undesirable effects
			// if multiple thread query this epoll object.
			boost::intrusive::list<Item> stolen;
			stolen.splice(stolen.end(), _pendingQueue);

			while(!stolen.empty()) {
				auto item = &stolen.front();
				stolen.pop_front();
				assert(item->isPending);

				auto result = COFIBER_AWAIT item->file->poll(0);
				auto status = std::get<2>(result) & item->eventMask;
				// TODO: In addition to watches without events,
				// edge-triggered watches should be discarded here.
				if(status) {
					_pendingQueue.push_back(*item);
					_pendingBell.ring();
				}else{
					item->isPending = false;

					// Once an item is not pending anymore, we continue watching it.
					auto poll = item->file->poll(std::get<0>(result));
					poll.then([item] (PollResult next_result) {
						_awaitPoll(item, next_result);
					});
				}
				
				if(!status)
					continue;

				struct epoll_event ev;
				ev.events = status;
				ev.data.u64 = item->cookie;
				COFIBER_RETURN(ev);
			}
		}
	}))

	// ------------------------------------------------------------------------
	// File implementation.
	// ------------------------------------------------------------------------

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *, size_t) override, ([=] {
		throw std::runtime_error("Cannot read from epoll FD");
	}))
	
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static constexpr auto fileOperations = protocols::fs::FileOperations{};

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	OpenFile()
	: ProxyFile{nullptr} { }

private:
	helix::UniqueLane _passthrough;

	// FIXME: This really has to map std::weak_ptrs or std::shared_ptrs.
	std::unordered_map<File *, Item *> _fileMap;

	boost::intrusive::list<Item> _pendingQueue;
	async::doorbell _pendingBell;
};

} // anonymous namespace

std::shared_ptr<ProxyFile> createEpollFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

void epollCtl(File *epfile, File *file, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->watchFile(file, flags, cookie);
}

async::result<struct epoll_event> epollWait(File *epfile) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvent();
}

