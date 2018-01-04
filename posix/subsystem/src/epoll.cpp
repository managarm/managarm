
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
	struct Watch : boost::intrusive::list_base_hook<> {
		Watch(OpenFile *epoll, File *file, int mask, uint64_t cookie)
		: _epoll{epoll}, _file{file}, _eventMask{mask}, _cookie{cookie},
				isPending{false} { }

		COFIBER_ROUTINE(cofiber::no_future, run(), ([=] {
			// Check the initial state of the file.
			auto initial = COFIBER_AWAIT _file->poll(0);
			if(_eventMask & std::get<2>(initial)) {
				isPending = true;
				_epoll->_pendingQueue.push_back(*this);
				_epoll->_pendingBell.ring();
			}

			// Wait until new edges occur.
			// TODO: We do not need these poll() calls while isPending is true
			// as checkEvents() will be called anyway in this case.
			uint64_t sequence = std::get<0>(initial);
			while(true) {
				auto result = COFIBER_AWAIT _file->poll(sequence);
				
				if(!isPending) {
					isPending = true;
					_epoll->_pendingQueue.push_back(*this);
					_epoll->_pendingBell.ring();
				}

				sequence = std::get<0>(result);
			}
		}))

		uint64_t cookie() {
			return _cookie;
		}
		
		COFIBER_ROUTINE(async::result<int>, checkEvents(), ([=] {
			auto result = COFIBER_AWAIT _file->poll(0);
			COFIBER_RETURN(_eventMask & std::get<2>(result));
		}))


	private:
		OpenFile *_epoll;
		File *_file;
		int _eventMask;
		uint64_t _cookie;

	public:
		bool isPending;
	};

public:
	~OpenFile() {
		assert(!"close() does not work correctly for epoll files");
	}

	void watchEvent(File *file, int mask, uint64_t cookie) {
		// TODO: Fix the memory-leak.
		assert(_watches.find(file) == _watches.end());
		auto watch = new Watch{this, file, mask, cookie};
		watch->run();
		_watches.insert({file, watch});
	}

	COFIBER_ROUTINE(async::result<struct epoll_event>, waitForEvent(), ([=] {
		while(true) {
			while(_pendingQueue.empty())
				COFIBER_AWAIT _pendingBell.async_wait();

			// TODO: Stealing all elements might lead to undesirable effects
			// if multiple thread query this epoll object.
			boost::intrusive::list<Watch> stolen;
			stolen.splice(stolen.end(), _pendingQueue);

			while(!stolen.empty()) {
				auto watch = &stolen.front();
				stolen.pop_front();
				assert(watch->isPending);

				auto events = COFIBER_AWAIT watch->checkEvents();
				// TODO: In addition to watches without events,
				// edge-triggered watches should be discarded here.
				if(events) {
					_pendingQueue.push_back(*watch);
					_pendingBell.ring();
				}else{
					watch->isPending = false;
				}
				
				if(!events)
					continue;

				struct epoll_event ev;
				ev.events = events;
				ev.data.u64 = watch->cookie();
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
	std::unordered_map<File *, Watch *> _watches;

	boost::intrusive::list<Watch> _pendingQueue;
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
	epoll->watchEvent(file, flags, cookie);
}

async::result<struct epoll_event> epollWait(File *epfile) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvent();
}

