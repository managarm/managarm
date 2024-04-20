#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <algorithm>
#include <deque>
#include <iostream>

#include <async/result.hpp>
#include <async/oneshot-event.hpp>
#include <boost/intrusive/list.hpp>
#include <helix/ipc.hpp>
#include <libevbackend.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include <frg/std_compat.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include "fs.bragi.hpp"
#include "hw.bragi.hpp"
#include "libevbackend.hpp"

namespace libevbackend {

bool logConfiguration = false;
bool logCodes = false;
bool logRequests = false;

namespace {

async::detached issueReset() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "pm-interface"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	auto pmLane = (co_await entity.getRemoteLane()).unwrap();

	managarm::hw::PmResetRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			pmLane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);
	recv_head.reset();

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	throw std::runtime_error("Return from PM_RESET request");
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// File implementation.
// ----------------------------------------------------------------------------

async::result<protocols::fs::ReadResult>
File::read(void *object, const char *, void *buffer, size_t max_size) {
	auto self = static_cast<File *>(object);

	// Make sure that we can at least write the SYN_DROPPED packet.
	if(max_size < sizeof(input_event))
		co_return protocols::fs::Error::illegalArguments;

	if(self->_nonBlock && self->_pending.empty() && !self->_overflow)
		co_return protocols::fs::Error::wouldBlock;

	while(self->_pending.empty() && !self->_overflow)
		co_await self->_statusBell.async_wait();

	if(self->_overflow) {
		struct timespec now;
		if(clock_gettime(self->_clockId, &now))
			throw std::runtime_error("clock_gettime() failed");

		input_event uev;
		memset(&uev, 0, sizeof(input_event));
		uev.time.tv_sec = now.tv_sec;
		uev.time.tv_usec = now.tv_nsec / 1000;
		uev.type = EV_SYN;
		uev.code = SYN_DROPPED;
		memcpy(reinterpret_cast<char *>(buffer), &uev, sizeof(input_event));

		// Reset the overflow flag.
		self->_pending.clear();
		self->_overflow = false;

		co_return sizeof(input_event);
	}else{
		size_t written = 0;
		while(!self->_pending.empty()
				&& written + sizeof(input_event) <= max_size) {
			auto evt = self->_pending.front();
			self->_pending.pop_front();
			if(self->_pending.empty())
				self->_statusPage.update(self->_currentSeq, 0);

			input_event uev;
			memset(&uev, 0, sizeof(input_event));
			uev.time.tv_sec = evt.timestamp.tv_sec;
			uev.time.tv_usec = evt.timestamp.tv_nsec / 1000;
			uev.type = evt.type;
			uev.code = evt.code;
			uev.value = evt.value;
			memcpy(reinterpret_cast<char *>(buffer) + written, &uev, sizeof(input_event));
			written += sizeof(input_event);
		}

		assert(written);
		co_return written;
	}
}


async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
File::pollWait(void *object, uint64_t past_seq, int mask,
		async::cancellation_token) {
	(void) mask;

	auto self = static_cast<File *>(object);

	assert(past_seq <= self->_currentSeq);
	while(self->_currentSeq == past_seq)
		co_await self->_statusBell.async_wait();

	co_return protocols::fs::PollWaitResult{
		self->_currentSeq,
		self->_currentSeq > 0 ? EPOLLIN : 0
	};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
File::pollStatus(void *object) {
	auto self = static_cast<File *>(object);

	co_return protocols::fs::PollStatusResult{
		self->_currentSeq,
		self->_pending.empty() ? 0 : EPOLLIN
	};
}

async::result<void>
File::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	if(id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
		assert(req);
		auto self = static_cast<File *>(object);
		if(req->command() == EVIOCGBIT(0, 0)) {
			assert(req->size());
			if(logRequests)
				std::cout << "EVIOCGBIT()" << std::endl;

			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto chunk = std::min(size_t(req->size()), self->_device->_typeBits.size());
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(self->_device->_typeBits.data(), chunk)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req->command() == EVIOCGBIT(1, 0)) {
			assert(req->size());
			if(logRequests)
				std::cout << "EVIOCGBIT(" << req->input_type() << ")" << std::endl;

			managarm::fs::GenericIoctlReply resp;

			std::pair<const uint8_t *, size_t> p;
			if(req->input_type() == EV_KEY) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_keyBits.data(), self->_device->_keyBits.size()};
			}else if(req->input_type() == EV_REL) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_relBits.data(), self->_device->_relBits.size()};
			}else if(req->input_type() == EV_ABS) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_absBits.data(), self->_device->_absBits.size()};
			}else{
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {nullptr, 0};
			}

			auto ser = resp.SerializeAsString();
			auto chunk = std::min(size_t(req->size()), p.second);
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(p.first, chunk)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req->command() == EVIOCSCLOCKID) {
			managarm::fs::GenericIoctlReply resp;

			// TODO: Does this setting affect already queued events in Linux?
			switch(req->input_clock()) {
			case CLOCK_REALTIME:
			case CLOCK_MONOTONIC:
				self->_clockId = req->input_clock();
				resp.set_error(managarm::fs::Errors::SUCCESS);
				break;
			default:
				assert(!"Clock is not supported in libevbackend");
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req->command() == EVIOCGABS(0)) {
			managarm::fs::GenericIoctlReply resp;
			if(logRequests)
				std::cout << "EVIOCGABS(" << req->input_type() << ")" << std::endl;

			assert(static_cast<size_t>(req->input_type())
					< self->_device->_absoluteSlots.size());
			auto slot = &self->_device->_absoluteSlots[req->input_type()];
			resp.set_input_value(slot->value);
			resp.set_input_min(slot->minimum);
			resp.set_input_max(slot->maximum);
			resp.set_input_fuzz(0);
			resp.set_input_flat(0);
			resp.set_input_resolution(1);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "Unknown ioctl() with ID " << std::to_string(req->command()) << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}else{
		std::cout << "Unknown ioctl() message with ID " << id << std::endl;
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}


constexpr auto fileOperations = protocols::fs::FileOperations{
	.read = &File::read,
	.ioctl = &File::ioctl,
	.pollWait = &File::pollWait,
	.pollStatus = &File::pollStatus
};

helix::UniqueLane File::serve(smarter::shared_ptr<File> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(
			std::move(local_lane), file, &fileOperations));
	return remote_lane;
}

File::File(EventDevice *device, bool non_block)
: _device{device}, _currentSeq{1}, _nonBlock{non_block}, _clockId{CLOCK_MONOTONIC} {
	_statusPage.update(_currentSeq, 0);
}

File::~File() {
	// TODO: This should probably be done in an explicit handleClose().
	_device->_files.erase(_device->_files.iterator_to(*this));
}

// ----------------------------------------------------------------------------
// EventDevice implementation.
// ----------------------------------------------------------------------------

async::detached serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane lane) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		recv_req.reset();
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			auto file = smarter::make_shared<File>(device.get(),
					req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK);
			device->_files.push_back(*file.get());
			auto remote_lane = File::serve(file);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FileCaps::FC_STATUS_PAGE);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_pt, push_page] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane),
				helix_ng::pushDescriptor(file->_statusPage.getMemory())
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else{
			throw std::runtime_error("Invalid serveDevice request!");
		}
	}
}

EventDevice::EventDevice() {
	memset(_typeBits.data(), 0, _typeBits.size());
	memset(_keyBits.data(), 0, _keyBits.size());
	memset(_relBits.data(), 0, _relBits.size());
	memset(_absBits.data(), 0, _absBits.size());

	memset(_currentKeys.data(), 0, _currentKeys.size());
	memset(_absoluteSlots.data(), 0, _absoluteSlots.size() * sizeof(AbsoluteSlot));
}

void EventDevice::setAbsoluteDetails(int code, int minimum, int maximum) {
	assert(static_cast<size_t>(code) < _absoluteSlots.size());
	_absoluteSlots[code].minimum = minimum;
	_absoluteSlots[code].maximum = maximum;
}

void EventDevice::enableEvent(int type, int code) {
	auto setBit = [] (uint8_t *array, size_t length, unsigned int bit) {
		assert(bit / 8 < length);
		array[bit / 8] |= (1 << (bit % 8));
	};
	if(logConfiguration)
		std::cout << "drivers/libevbackend: Enabling event " << type << "." << code
				<< std::endl;

	if(type == EV_KEY) {
		setBit(_keyBits.data(), _keyBits.size(), code);
	}else if(type == EV_REL) {
		setBit(_relBits.data(), _relBits.size(), code);
	}else if(type == EV_ABS) {
		setBit(_absBits.data(), _absBits.size(), code);
	}else{
		throw std::runtime_error("Unexpected event type");
	}
	setBit(_typeBits.data(), _typeBits.size(), type);
}

void EventDevice::emitEvent(int type, int code, int value) {
	auto getBit = [] (uint8_t *array, size_t length, unsigned int bit) -> bool {
		assert(bit / 8 < length);
		return array[bit / 8] & (1 << (bit % 8));
	};
	auto putBit = [] (uint8_t *array, size_t length, unsigned int bit, bool value) {
		assert(bit / 8 < length);
		array[bit / 8] &= ~(1 << (bit % 8));
		array[bit / 8] |= (((int)value) << (bit % 8));
	};

	// Filter out events that do not update the device state.
	if(type == EV_KEY && getBit(_currentKeys.data(), _currentKeys.size(), code) == value)
		return;
	if(type == EV_REL && !value)
		return;
	if(type == EV_ABS && value == _absoluteSlots[code].value)
		return;

	// Update the device state.
	if(type == EV_KEY) {
		putBit(_currentKeys.data(), _currentKeys.size(), code, value);
	}else if(type == EV_ABS) {
		_absoluteSlots[code].value = value;
	}

	// Handle magic key sequences in the driver.  This ensure that all devices implement
	// the same magic keys. It is also more reliable than implementing this in a second process.
	static bool resetSent = false;
	if(!resetSent
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_LEFTCTRL)
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_LEFTALT)
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_DELETE)) {
		std::cout << "drivers/libevbackend: Issuing CTRL+ALT+DEL reset" << std::endl;
		issueReset();
		resetSent = true;
	}

	_staged.push_back(StagedEvent{type, code, value});
}

void EventDevice::notify() {
	if(_staged.empty())
		return;

	for(auto &file : _files) {
		if(file._overflow)
			continue;

		struct timespec now;
		if(clock_gettime(file._clockId, &now))
			throw std::runtime_error("clock_gettime() failed");

		if(file._pending.size() > 1024) {
			file._overflow = true;
			continue;
		}

		if(logCodes)
			for(StagedEvent evt : _staged)
				std::cout << "[" << now.tv_sec << "." << (now.tv_nsec / 1'000'000)
						<< "] Event type: " << evt.type << ", code: " << evt.code
						<< ", value: " << evt.value << std::endl;

		for(StagedEvent evt : _staged)
			file._pending.push_back(PendingEvent{evt.type, evt.code, evt.value, now});
		file._currentSeq++;
		file._statusPage.update(file._currentSeq, EPOLLIN);
		file._statusBell.raise();
	}
	_staged.clear();
}

} // namespace libevbackend
