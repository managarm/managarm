
#ifndef LIBEVBACKEND_HPP
#define LIBEVBACKEND_HPP

#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

namespace libevbackend {

// --------------------------------------------
// Event
// --------------------------------------------

struct Event {
	Event(int type, int code, int value)
	: type(type), code(code), value(value) { }

	int type;
	int code;
	int value;
	boost::intrusive::list_member_hook<> hook;
};

// --------------------------------------------
// ReadRequest
// --------------------------------------------

struct ReadRequest {
	ReadRequest(void *buffer, size_t maxLength)
	: buffer(buffer), maxLength(maxLength) { }

	void *buffer;
	size_t maxLength;
	async::promise<size_t> promise;
	boost::intrusive::list_member_hook<> hook;
};

// --------------------------------------------
// EventDevice
// --------------------------------------------

struct EventDevice {
	static async::result<void> seek(std::shared_ptr<void> object, uintptr_t offset);
	static async::result<size_t> read(std::shared_ptr<void> object, void *buffer, size_t length);
	static async::result<void> write(std::shared_ptr<void> object, const void *buffer, size_t length);
	static async::result<helix::BorrowedDescriptor> accessMemory(std::shared_ptr<void> object);

	void emitEvent(int type, int code, int value);

private:
	void _processEvents();

	boost::intrusive::list<
		Event,
		boost::intrusive::member_hook<
			Event,
			boost::intrusive::list_member_hook<>,
			&Event::hook
		>
	> _events;

	boost::intrusive::list<
		ReadRequest,
		boost::intrusive::member_hook<
			ReadRequest,
			boost::intrusive::list_member_hook<>,
			&ReadRequest::hook
		>
	> _requests;
};

constexpr protocols::fs::FileOperations fileOperations {
	&EventDevice::seek,
	&EventDevice::read,
	&EventDevice::write,
	&EventDevice::accessMemory
};

// --------------------------------------------
// Functions
// --------------------------------------------

cofiber::no_future serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane p);

} // namespace libevbackend

#endif // LIBEVBACKEND_HPP
