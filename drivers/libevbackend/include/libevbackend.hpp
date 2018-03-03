
#ifndef LIBEVBACKEND_HPP
#define LIBEVBACKEND_HPP

#include <async/result.hpp>
#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

namespace libevbackend {

struct EventDevice;

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
// EventDevice
// --------------------------------------------

struct File {
	// ------------------------------------------------------------------------
	// File operations.
	// ------------------------------------------------------------------------
	
	static async::result<protocols::fs::ReadResult>
	read(void *object, void *buffer, size_t length);

	static async::result<void>
	write(void *object, const void *buffer, size_t length);

	static async::result<protocols::fs::PollResult>
	poll(void *object, uint64_t past_seq);

	static async::result<void>
	ioctl(void *object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);
	
	// ------------------------------------------------------------------------
	// Public File API.
	// ------------------------------------------------------------------------

	static helix::UniqueLane serve(std::shared_ptr<File> file);

	File(EventDevice *device, bool non_block);

private:
	EventDevice *_device;
	bool _nonBlock;
};

struct EventDevice {
	friend struct File;

	EventDevice();

	void enableEvent(int type, int code);

	void emitEvent(int type, int code, int value);

	void notify();

private:
	async::doorbell _statusBell;
	uint64_t _currentSeq;

	// Supported event bits.
	// The array sizes come from Linux' EV_CNT, KEY_CNT, REL_CNT etc. macros (divided by 8)
	// and can be extended if more event constants are added.
	std::array<uint8_t, 4> _typeBits;
	std::array<uint8_t, 96> _keyBits;
	std::array<uint8_t, 2> _relBits;

	boost::intrusive::list<
		Event,
		boost::intrusive::member_hook<
			Event,
			boost::intrusive::list_member_hook<>,
			&Event::hook
		>
	> _emitted;

	boost::intrusive::list<
		Event,
		boost::intrusive::member_hook<
			Event,
			boost::intrusive::list_member_hook<>,
			&Event::hook
		>
	> _events;
};

// --------------------------------------------
// Functions
// --------------------------------------------

cofiber::no_future serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane p);

} // namespace libevbackend

#endif // LIBEVBACKEND_HPP
