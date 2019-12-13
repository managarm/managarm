#pragma once

#include <queue>
#include <vector>

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
// Events
// --------------------------------------------

struct StagedEvent {
	int type;
	int code;
	int value;
};

struct PendingEvent {
	int type;
	int code;
	int value;
	struct timespec timestamp;
};

// --------------------------------------------
// EventDevice
// --------------------------------------------

struct File {
	friend struct EventDevice;
	friend async::detached serveDevice(std::shared_ptr<EventDevice>, helix::UniqueLane);

	// ------------------------------------------------------------------------
	// File operations.
	// ------------------------------------------------------------------------

	static async::result<protocols::fs::ReadResult>
	read(void *object, const char *, void *buffer, size_t length);

	static async::result<void>
	write(void *object, const char *, const void *buffer, size_t length);

	static async::result<protocols::fs::PollResult>
	poll(void *object, uint64_t past_seq, async::cancellation_token cancellation);

	static async::result<void>
	ioctl(void *object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);

	// ------------------------------------------------------------------------
	// Public File API.
	// ------------------------------------------------------------------------

	static helix::UniqueLane serve(smarter::shared_ptr<File> file);

	File(EventDevice *device, bool non_block);

	~File();

private:
	EventDevice *_device;
	boost::intrusive::list_member_hook<> hook;
	protocols::fs::StatusPageProvider _statusPage;
	async::doorbell _statusBell;
	uint64_t _currentSeq;

	bool _nonBlock;
	// Clock ID for input timestamps.
	int _clockId;

	std::queue<PendingEvent> _pending;
};

struct EventDevice {
	struct AbsoluteSlot {
		int minimum;
		int maximum;

		int value;
	};

public:
	friend struct File;
	friend async::detached serveDevice(std::shared_ptr<EventDevice>, helix::UniqueLane);

	EventDevice();

	void setAbsoluteDetails(int code, int minimum, int maximum);

	void enableEvent(int type, int code);

	void emitEvent(int type, int code, int value);

	void notify();

private:
	// Supported event bits.
	// The array sizes come from Linux' EV_CNT, KEY_CNT, REL_CNT etc. macros (divided by 8)
	// and can be extended if more event constants are added.
	std::array<uint8_t, 4> _typeBits;
	std::array<uint8_t, 96> _keyBits;
	std::array<uint8_t, 2> _relBits;
	std::array<uint8_t, 8> _absBits;

	// Input details and current input state.
	std::array<uint8_t, 96> _currentKeys;
	std::array<AbsoluteSlot, 8> _absoluteSlots;

	boost::intrusive::list<
		File,
		boost::intrusive::member_hook<
			File,
			boost::intrusive::list_member_hook<>,
			&File::hook
		>
	> _files;

	std::vector<StagedEvent> _staged;
};

// --------------------------------------------
// Functions
// --------------------------------------------

async::detached serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane p);

} // namespace libevbackend
