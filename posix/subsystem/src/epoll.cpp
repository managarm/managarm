
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "common.hpp"
#include "epoll.hpp"

namespace {

bool logEpoll = false;

struct OpenFile : File {
	// ------------------------------------------------------------------------
	// Internal API.
	// ------------------------------------------------------------------------
private:
	// Lifetime management: There are the following three state bits for each item.
	// Items are deleted once all state bits are zero.
	// Items must only be accessed while a precondition guarantees that
	// at least one state bit is non-zero.
	using State = uint32_t;
	static constexpr State stateActive = 1;
	static constexpr State statePolling = 2;
	static constexpr State statePending = 4;

	struct Item : boost::intrusive::list_base_hook<> {
		Item(OpenFile *epoll, File *file, int mask, uint64_t cookie)
		: epoll{epoll}, state{stateActive},
				file{file}, eventMask{mask}, cookie{cookie} { }

		OpenFile *epoll;
		State state;

		// Basic data of this item.
		File *file;
		int eventMask;
		uint64_t cookie;
	};

	static void _awaitPoll(Item *item, PollResult result) {
		assert(item->state & statePolling);
		auto self = item->epoll;

		// Collect non-active items without polling again.
		if(!(item->state & stateActive)) {
			item->state &= ~statePolling;
			if(!item->state)
				delete item;
			return;
		}
		
		// TODO: Ignore items that are already pending.
		// This will happen once we implement modifyItem().
		assert(!(item->state & statePending));

		// Note that items only become pending if there is an edge.
		// This is the correct behavior for edge-triggered items.
		// Level-triggered items stay pending until the event disappears.
		if((std::get<1>(result) & item->eventMask)
				&& (std::get<2>(result) & item->eventMask)) {
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m becomes pending" << std::endl;

			// Note that we stop watching once an item becomes pending.
			// We do this as we have to poll() again anyway before we report the item.
			item->state &= ~statePolling;
			item->state |= statePending;

			self->_pendingQueue.push_back(*item);
			self->_currentSeq++;
			self->_pendingBell.ring();
		}else{
			// Here, we assume that the lambda does not execute on the current stack.
			// TODO: Use some callback queueing mechanism to ensure this.
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m still not pending after poll()."
						<< " Mask is " << item->eventMask << ", while "
						<< std::get<2>(result) << " is active" << std::endl;
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

	void addItem(File *file, int mask, uint64_t cookie) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Adding item \e[1;34m"
					<< file->structName() << "\e[0m. Mask is " << mask << std::endl;
		// TODO: Fix the memory-leak.
		assert(_fileMap.find(file) == _fileMap.end());
		auto item = new Item{this, file, mask, cookie};

		item->state |= statePolling;
		auto poll = item->file->poll(0);
		poll.then([item] (PollResult result) {
			_awaitPoll(item, result);
		});

		_fileMap.insert({file, item});
	}
	
	void modifyItem(File *file, int mask, uint64_t cookie) {
		std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Modifying item \e[1;34m"
				<< file->structName() << "\e[0m. New mask is " << mask << std::endl;
	}

	void deleteItem(File *file) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Deleting item \e[1;34m"
					<< file->structName() << "\e[0m" << std::endl;
		auto it = _fileMap.find(file);
		assert(it != _fileMap.end());

		it->second->state &= ~stateActive;
		if(!it->second->state)
			delete it->second;
	}

	COFIBER_ROUTINE(async::result<size_t>, waitForEvents(struct epoll_event *events,
			size_t max_events), ([=] {
		assert(max_events);
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Entering wait."
					" There are " << _pendingQueue.size() << " pending items" << std::endl;
		size_t k = 0;
		boost::intrusive::list<Item> repoll_queue;
		while(!k) {
			while(_pendingQueue.empty())
				COFIBER_AWAIT _pendingBell.async_wait();

			while(!_pendingQueue.empty()) {
				auto item = &_pendingQueue.front();
				_pendingQueue.pop_front();
				assert(item->state & statePending);

				// Collect non-alive items without returning them.
				if(!(item->state & stateActive)) {
					item->state &= ~statePending;
					if(!item->state)
						delete item;
					continue;
				}

				auto result = COFIBER_AWAIT item->file->poll(0);	
				if(logEpoll)
					std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Checking item "
							<< "\e[1;34m" << item->file->structName() << "\e[0m."
							" Mask is " << item->eventMask << ", while " << std::get<2>(result)
							<< " is active" << std::endl;
				auto status = std::get<2>(result) & item->eventMask;

				// Abort early (i.e before requeuing) if the item is not pending.
				if(!status) {
					// TODO: Once we implement modifyItem(), items can be both
					// polling and pending at the same time. In this case, only poll
					// if we are not already polling.
					assert(!(item->state & statePolling));
					item->state &= ~statePending;
					item->state |= statePolling;

					// Once an item is not pending anymore, we continue watching it.
					auto poll = item->file->poll(std::get<0>(result));
					poll.then([item] (PollResult next_result) {
						_awaitPoll(item, next_result);
					});

					continue;
				}
				
				// We have to increment the sequence again as concurrent waiters
				// might have seen an empty _pendingQueue.
				// TODO: Edge-triggered watches should not be requeued here.
				repoll_queue.push_back(*item);

				memset(events + k, 0, sizeof(struct epoll_event));
				events[k].events = status;
				events[k].data.u64 = item->cookie;

				k++;
				if(k == max_events)
					break;
			}
		}

		// Before returning, we have to reinsert the level-triggered events that we report.
		if(!repoll_queue.empty()) {
			_pendingQueue.splice(_pendingQueue.end(), repoll_queue);
			_currentSeq++;
			_pendingBell.ring();
		}

		COFIBER_RETURN(k);
	}))

	// ------------------------------------------------------------------------
	// File implementation.
	// ------------------------------------------------------------------------

	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t past_seq) override, ([=] {
		assert(past_seq <= _currentSeq);
		while(_currentSeq == past_seq)
			COFIBER_AWAIT _pendingBell.async_wait();

		COFIBER_RETURN(PollResult(_currentSeq, EPOLLIN, _pendingQueue.empty() ? 0 : EPOLLIN));
	}))
	
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), std::shared_ptr<File>{file},
				&File::fileOperations);
	}

	OpenFile()
	: File{StructName::get("epoll")}, _currentSeq{1} { }

private:
	helix::UniqueLane _passthrough;

	// FIXME: This really has to map std::weak_ptrs or std::shared_ptrs.
	std::unordered_map<File *, Item *> _fileMap;

	boost::intrusive::list<Item> _pendingQueue;
	async::doorbell _pendingBell;
	uint64_t _currentSeq;
};

} // anonymous namespace

namespace epoll {

std::shared_ptr<File> createFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

void addItem(File *epfile, File *file, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->addItem(file, flags, cookie);
}

void modifyItem(File *epfile, File *file, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->modifyItem(file, flags, cookie);
}

void deleteItem(File *epfile, File *file, int flags) {
	assert(!flags);
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->deleteItem(file);
}

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvents(events, max_events);
}

} // namespace epoll

