
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include "timerfd.hpp"

namespace {

struct OpenFile : ProxyFile {
	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static async::result<size_t> ptRead(std::shared_ptr<void> object,
			void *buffer, size_t length) {
		auto self = static_cast<OpenFile *>(object.get());
		return self->readSome(buffer, length);
	}
	
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withRead(&ptRead);

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	OpenFile()
	: ProxyFile{nullptr}, _expirations{0}, _theSeq{1} { }

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(max_length == sizeof(uint64_t));

		memcpy(data, &_expirations, sizeof(uint64_t));
		_expirations = 0;
		COFIBER_RETURN(sizeof(uint64_t));
	}))
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t in_seq) override, ([=] {
		std::cout << "posix: timerfd::poll(" << in_seq << ")" << std::endl;
		assert(in_seq <= _theSeq);
		while(in_seq == _theSeq)
			COFIBER_AWAIT _seqBell.async_wait();

		COFIBER_RETURN(PollResult(_theSeq, EPOLLIN, _expirations ? EPOLLIN : 0));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}
	
	COFIBER_ROUTINE(cofiber::no_future, setTime(struct timespec time), ([=] {
		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

//		std::cout << "posix: Timer armed" << std::endl;

		helix::AwaitClock await_clock;
		auto &&submit = helix::submitAwaitClock(&await_clock, tick + time.tv_nsec,
				helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_clock.error());

//		std::cout << "posix: Timer expiration" << std::endl;
		_expirations++;

		_theSeq++;
		_seqBell.ring();
	}))

private:
	helix::UniqueLane _passthrough;

	// Number of expirations since last read().
	uint64_t _expirations;

	uint64_t _theSeq;
	async::doorbell _seqBell;
};

} // anonymous namespace

namespace timerfd {

std::shared_ptr<ProxyFile> createFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

void setTime(File *file, struct timespec initial, struct timespec interval) {
	assert(!interval.tv_sec);
	assert(!interval.tv_nsec);

	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->setTime(initial);
}

} // namespace timerfd
