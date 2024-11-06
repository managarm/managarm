#pragma once

#include <map>
#include <vector>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <boost/intrusive/list.hpp>
#include <helix/ipc.hpp>
#include <linux/input.h>
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

constexpr int ABS_MT_FIRST = ABS_MT_SLOT;
constexpr int ABS_MT_LAST = ABS_MT_TOOL_Y;
constexpr size_t maxMultitouchSlots = 10;

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

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t past_seq, int mask,
			async::cancellation_token cancellation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	static async::result<void>
	ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult,
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
	async::recurring_event _statusBell;
	uint64_t _currentSeq;

	bool _nonBlock;
	// Clock ID for input timestamps.
	int _clockId;

	std::deque<PendingEvent> _pending;
	bool _overflow = false;
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

	EventDevice(std::string name, uint16_t bustype, uint16_t vendor, uint16_t product);

	void setAbsoluteDetails(int code, int minimum, int maximum);

	void enableEvent(int type, int code);

	void emitEvent(int type, int code, int value);

	void notify();

	struct multitouchInfo {
		// the multitouch tracking ID exposed to userspace
		int userTrackingId = -1;

		// the current values of multitouch codes
		// only multitouch codes are stored, therefore indices are offset by the lowest code
		// index 0 is ABS_MT_FIRST, 1 is (ABS_MT_FIRST + 1), ...
		std::array<int, (ABS_MT_LAST - ABS_MT_FIRST + 1)> abs = {};
	};

	const std::map<int, multitouchInfo> &currentMultitouchState() {
		return _mtState;
	}

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
	std::array<AbsoluteSlot, 64> _absoluteSlots;

	// current multitouch state, keyed by slot ID
	std::map<int, multitouchInfo> _mtState;

	boost::intrusive::list<
		File,
		boost::intrusive::member_hook<
			File,
			boost::intrusive::list_member_hook<>,
			&File::hook
		>
	> _files;

	std::vector<StagedEvent> _staged;

	std::string name_;
	uint16_t busType_;
	uint16_t vendor_;
	uint16_t product_;
};

// --------------------------------------------
// Functions
// --------------------------------------------

async::detached serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane p);

} // namespace libevbackend
