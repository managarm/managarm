
#include <string.h>
#include <iostream>

#include <async/recurring-event.hpp>
#include <boost/intrusive/list.hpp>
#include <frg/manual_box.hpp>
#include <helix/ipc.hpp>
#include "common.hpp"
#include "epoll.hpp"
#include "fs.hpp"

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

	struct Item;

	struct Receiver {
		void set_value_inline(frg::expected<Error, PollWaitResult> outcome) {
			item->pollOutcome.emplace(std::move(outcome));
		}

		void set_value_noinline(frg::expected<Error, PollWaitResult> outcome) {
			item->pollOutcome.emplace(std::move(outcome));
			_awaitPoll(item.get());
		}

		smarter::shared_ptr<Item> item;
	};

	struct Item : boost::intrusive::list_base_hook<> {
		Item(smarter::shared_ptr<OpenFile> epoll, Process *process,
				smarter::shared_ptr<File> file, int mask, uint64_t cookie)
		: epoll{epoll}, state{stateActive}, process{process},
				file{std::move(file)}, eventMask{mask}, cookie{cookie} { }

		smarter::shared_ptr<OpenFile> epoll;
		State state;

		// Basic data of this item.
		Process *process;
		smarter::shared_ptr<File> file;
		int eventMask;
		uint64_t cookie;

		async::cancellation_event cancelPoll;

		frg::manual_box<
			async::execution::operation_t<
					async::result<frg::expected<Error, PollWaitResult>>,
					Receiver
			>
		> pollOperation;

		std::optional<frg::expected<Error, PollWaitResult>> pollOutcome;

		smarter::borrowed_ptr<Item> self;
	};

	static void _awaitPoll(Item *item) {
	reRunImmediately:
		// First, destruct the operation so that we can re-use it later.
		item->pollOperation.destruct();

		assert(item->state & statePolling);
		auto self = item->epoll.get();

		// Discard non-active and closed items.
		if(!(item->state & stateActive)) {
			item->state &= ~statePolling;
			// TODO: We might have polling + pending items in the future.
			assert(!item->state);
			return;
		}

		auto resultOrError = std::move(*item->pollOutcome);

		if(!resultOrError) {
			assert(resultOrError.error() == Error::fileClosed);
			item->state &= ~statePolling;
			return;
		}

		// Note that items only become pending if there is an edge.
		// This is the correct behavior for edge-triggered items.
		// Level-triggered items stay pending until the event disappears.
		auto result = resultOrError.value();
		if(std::get<1>(result) & (item->eventMask | EPOLLERR | EPOLLHUP)) {
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m becomes pending" << std::endl;

			// Note that we stop watching once an item becomes pending.
			// We do this as we have to pollStatus() again anyway before we report the item.
			item->state &= ~statePolling;
			if(!(item->state & statePending)) {
				item->state |= statePending;

				item->self.lock().ctr()->increment();
				self->_pendingQueue.push_back(*item);
				self->_currentSeq++;
				self->_statusBell.raise();
			}
		}else{
			// Here, we assume that the lambda does not execute on the current stack.
			// TODO: Use some callback queueing mechanism to ensure this.
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m still not pending after pollWait()."
						<< " Mask is " << item->eventMask << ", while edges are "
						<< std::get<1>(result) << std::endl;
			item->cancelPoll.reset();
			item->pollOperation.construct_with([&] {
				return async::execution::connect(
					item->file->pollWait(item->process, std::get<0>(result),
							item->eventMask | EPOLLERR | EPOLLHUP, item->cancelPoll),
					Receiver{item->self.lock()}
				);
			});
			// Poll should not return immediately; we use an ugly goto here in favor of wrapping
			// the entire function in a loop.
			if(async::execution::start_inline(*item->pollOperation))
				goto reRunImmediately;
		}
	}

public:
	~OpenFile() {
		// Nothing to do here.
	}

	Error addItem(Process *process, smarter::shared_ptr<File> file, int fd,
			int mask, uint64_t cookie) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Adding item \e[1;34m"
					<< file->structName() << "\e[0m. Mask is " << mask << std::endl;
		// TODO: Fix the memory-leak.
		if(_fileMap.find({file.get(), fd}) != _fileMap.end()) {
			return Error::alreadyExists;
		}

		auto item = smarter::make_shared<Item>(smarter::static_pointer_cast<OpenFile>(weakFile().lock()),
				process, std::move(file), mask, cookie);
		item->self = item;

		item->state |= statePending;

		_fileMap.insert({{item->file.get(), fd}, item});

		item.ctr()->increment();
		_pendingQueue.push_back(*item);
		_currentSeq++;
		_statusBell.raise();
		return Error::success;
	}

	Error modifyItem(File *file, int fd, int mask, uint64_t cookie) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Modifying item \e[1;34m"
					<< file->structName() << "\e[0m. New mask is " << mask << std::endl;
		auto it = _fileMap.find({file, fd});
		if(it == _fileMap.end()) {
			return Error::noSuchFile;
		}
		auto item = it->second;
		assert(item->state & stateActive);

		item->eventMask = mask;
		item->cookie = cookie;
		item->cancelPoll.cancel();

		// Mark the item as pending.
		if(!(item->state & statePending)) {
			item->state |= statePending;

			item.ctr()->increment();
			_pendingQueue.push_back(*item);
			_currentSeq++;
			_statusBell.raise();
		}
		return Error::success;
	}

	Error deleteItem(File *file, int fd) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Deleting item \e[1;34m"
					<< file->structName() << "\e[0m" << std::endl;
		auto it = _fileMap.find({file, fd});
		if(it == _fileMap.end()) {
			return Error::noSuchFile;
		}
		auto item = it->second;
		assert(item->state & stateActive);

		item->cancelPoll.cancel();

		_fileMap.erase(it);
		item->state &= ~stateActive;
		return Error::success;
	}

	async::result<size_t>
	waitForEvents(struct epoll_event *events, size_t max_events,
			async::cancellation_token cancellation) {
		assert(max_events);
		if(logEpoll) {
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Entering wait."
					" There are " << _pendingQueue.size() << " pending items; cancellation is "
					<< (cancellation.is_cancellation_requested() ? "active" : "inactive")
					<< std::endl;
		}

		size_t k = 0;
		boost::intrusive::list<Item> repoll_queue;
		while(true) {
			// TODO: Stop waiting in this case.
			assert(isOpen());

			while(!_pendingQueue.empty()) {
				auto item = _pendingQueue.front().self.lock();
				_pendingQueue.pop_front();
				item.ctr()->decrement();
				assert(item->state & statePending);

				// Discard non-alive items without returning them.
				if(!(item->state & stateActive)) {
					if(logEpoll)
						std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Discarding"
								" inactive item \e[1;34m" << item->file->structName() << "\e[0m"
								<< std::endl;
					item->state &= ~statePending;
					continue;
				}

				if(logEpoll)
					std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Checking item "
							<< "\e[1;34m" << item->file->structName() << "\e[0m" << std::endl;
				auto result_or_error = co_await item->file->pollStatus(item->process);

				// Discard closed items.
				if(!result_or_error) {
					assert(result_or_error.error() == Error::fileClosed);
					if(logEpoll)
						std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Discarding"
								" closed item \e[1;34m" << item->file->structName() << "\e[0m"
								<< std::endl;
					item->state &= ~statePending;
					continue;
				}

				auto result = result_or_error.value();
				if(logEpoll)
					std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m:"
							" Item \e[1;34m" << item->file->structName() << "\e[0m"
							" mask is " << item->eventMask << ", while " << std::get<1>(result)
							<< " is active" << std::endl;

				// Abort early (i.e before requeuing) if the item is not pending.
				auto status = std::get<1>(result) & (item->eventMask | EPOLLERR | EPOLLHUP);
				if(!status) {
					item->state &= ~statePending;
					if(!(item->state & statePolling)) {
						item->state |= statePolling;

						// Once an item is not pending anymore, we continue watching it.
						item->cancelPoll.reset();
						item->pollOperation.construct_with([&] {
							return async::execution::connect(
								item->file->pollWait(item->process, std::get<0>(result),
										item->eventMask | EPOLLERR | EPOLLHUP, item->cancelPoll),
								Receiver{item}
							);
						});
						if(async::execution::start_inline(*item->pollOperation))
							_awaitPoll(item.get());
					}
					continue;
				}

				// We have to increment the sequence again as concurrent waiters
				// might have seen an empty _pendingQueue.
				// TODO: Edge-triggered watches should not be requeued here.
				item.ctr()->increment();
				repoll_queue.push_back(*item);

				assert(k < max_events);
				memset(events + k, 0, sizeof(struct epoll_event));
				events[k].events = status;
				events[k].data.u64 = item->cookie;

				k++;
				if(k == max_events)
					break;
			}

			if(k)
				break;

			// Block and re-check if there are pending events.
			if(cancellation.is_cancellation_requested())
				break;
			co_await _statusBell.async_wait(cancellation);
		}

		// Before returning, we have to reinsert the level-triggered events that we report.
		if(!repoll_queue.empty()) {
			_pendingQueue.splice(_pendingQueue.end(), repoll_queue);
			_currentSeq++;
			_statusBell.raise();
		}

		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Return from wait"
					" with " << k << " items" << std::endl;

		co_return k;
	}

	// ------------------------------------------------------------------------
	// File implementation.
	// ------------------------------------------------------------------------

	void handleClose() override {
		auto it = _fileMap.begin();
		while(it != _fileMap.end()) {
			auto item = it->second;
			assert(item->state & stateActive);

			it = _fileMap.erase(it);
			item->state &= ~stateActive;

			if(item->state & statePolling)
				item->cancelPoll.cancel();

			if(item->state & statePending) {
				auto qit = _pendingQueue.iterator_to(*item);
				_pendingQueue.erase(qit);
				item->state &= ~statePending;
			}
		}

		_statusBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override {
		(void)mask; // TODO: utilize mask.
		assert(past_seq <= _currentSeq);
		while(_currentSeq == past_seq && !cancellation.is_cancellation_requested()) {
			assert(isOpen()); // TODO: Return a poll error here.
			co_await _statusBell.async_wait(cancellation);
		}
		if(cancellation.is_cancellation_requested())
			std::cout << "\e[33mposix: epoll::poll() cancellation is untested\e[39m" << std::endl;

		co_return PollWaitResult{_currentSeq, _currentSeq ? EPOLLIN : 0};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{_currentSeq, _pendingQueue.empty() ? 0 : EPOLLIN};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile()
	: File{StructName::get("epoll"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777), File::defaultPipeLikeSeek}, _currentSeq{0} { }

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// Since Item stores a strong pointer to each File,
	// it is sufficient if Key stores a plain (= non-owning) pointer.
	using Key = std::pair<File *, int>;

	struct KeyHash {
		size_t operator() (const Key &k) const {
			return 13 * reinterpret_cast<uintptr_t>(k.first)
					+ static_cast<size_t>(k.second);
		}
	};

	std::unordered_map<Key, smarter::shared_ptr<Item>, KeyHash> _fileMap;

	boost::intrusive::list<Item> _pendingQueue;
	async::recurring_event _statusBell;
	uint64_t _currentSeq;
};

} // anonymous namespace

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

Error addItem(File *epfile, Process *process, smarter::shared_ptr<File> file, int fd,
		int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->addItem(process, std::move(file), fd, flags, cookie);
}

Error modifyItem(File *epfile, File *file, int fd, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->modifyItem(file, fd, flags, cookie);
}

Error deleteItem(File *epfile, File *file, int fd, int flags) {
	assert(!flags);
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->deleteItem(file, fd);
}

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events, async::cancellation_token cancellation) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvents(events, max_events, cancellation);
}

} // namespace epoll

