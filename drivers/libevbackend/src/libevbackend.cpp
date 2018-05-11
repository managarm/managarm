
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <algorithm>
#include <deque>
#include <iostream>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <async/jump.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <libevbackend.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "fs.pb.h"
#include "hw.pb.h"
#include "libevbackend.hpp"

namespace libevbackend {

bool logCodes = false;
bool logRequests = false;

namespace {

async::jump pmFound;
helix::UniqueLane pmLane;

COFIBER_ROUTINE(cofiber::no_future, issueReset(), ([=] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "pm-interface")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		pmLane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		pmFound.trigger();
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
	COFIBER_AWAIT pmFound.async_wait();
	
	// Send the actual request.
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;

	managarm::hw::CntRequest req;
	req.set_req_type(managarm::hw::CntReqType::PM_RESET);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(pmLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::hw::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	throw std::runtime_error("Return from PM_RESET request");
}))

} // anonymous namespace

// ----------------------------------------------------------------------------
// File implementation.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<protocols::fs::ReadResult>,
File::read(void *object, const char *, void *buffer, size_t max_size), ([=] {
	auto self = static_cast<File *>(object);

	if(self->_nonBlock && self->_device->_events.empty())
		COFIBER_RETURN(protocols::fs::Error::wouldBlock);

	while(self->_device->_events.empty())
		COFIBER_AWAIT self->_device->_statusBell.async_wait();
	
	size_t written = 0;
	while(!self->_device->_events.empty()
			&& written + sizeof(input_event) <= max_size) {
		auto event = &self->_device->_events.front();
		self->_device->_events.pop_front();
		if(self->_device->_events.empty())
			self->_device->_statusPage.update(self->_device->_currentSeq, 0);

		// TODO: The time should be set when we enqueue the event.
		struct timespec now;
		if(clock_gettime(self->_clockId, &now))
			throw std::runtime_error("clock_gettime() failed");

		input_event uev;
		memset(&uev, 0, sizeof(input_event));
		uev.time.tv_sec = now.tv_sec;
		uev.time.tv_usec = now.tv_nsec / 1000;
		uev.type = event->type;
		uev.code = event->code;
		uev.value = event->value;

		memcpy(reinterpret_cast<char *>(buffer) + written, &uev, sizeof(input_event));
		written += sizeof(input_event);
		delete event;
	}
	
	assert(written);
	COFIBER_RETURN(written);
}))

async::result<void> File::write(void *, const char *, const void *, size_t) {
	throw std::runtime_error("write not yet implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::PollResult>,
File::poll(void *object, uint64_t past_seq), ([=] {
	auto self = static_cast<File *>(object);

	assert(past_seq <= self->_device->_currentSeq);
	while(self->_device->_currentSeq == past_seq)
		COFIBER_AWAIT self->_device->_statusBell.async_wait();
	
	COFIBER_RETURN(protocols::fs::PollResult(self->_device->_currentSeq, EPOLLIN,
			self->_device->_events.empty() ? 0 : EPOLLIN));
}))

COFIBER_ROUTINE(async::result<void>,
File::ioctl(void *object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([object = std::move(object),
		req = std::move(req), conversation = std::move(conversation)] {
	auto self = static_cast<File *>(object);
	if(req.command() == EVIOCGBIT(0, 0)) {
		assert(req.size());
		if(logRequests)
			std::cout << "EVIOCGBIT()" << std::endl;

		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto chunk = std::min(size_t(req.size()), self->_device->_typeBits.size());
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_data, self->_device->_typeBits.data(), chunk));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	}else if(req.command() == EVIOCGBIT(1, 0)) {
		assert(req.size());
		if(logRequests)
			std::cout << "EVIOCGBIT(" << req.input_type() << ")" << std::endl;

		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;
		managarm::fs::SvrResponse resp;

		std::pair<const uint8_t *, size_t> p;
		if(req.input_type() == EV_KEY) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			p = {self->_device->_keyBits.data(), self->_device->_keyBits.size()};
		}else if(req.input_type() == EV_REL) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			p = {self->_device->_relBits.data(), self->_device->_relBits.size()};
		}else if(req.input_type() == EV_ABS) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			p = {self->_device->_absBits.data(), self->_device->_absBits.size()};
		}else{
			resp.set_error(managarm::fs::Errors::SUCCESS);
			p = {nullptr, 0};
		}
	
		auto ser = resp.SerializeAsString();
		auto chunk = std::min(size_t(req.size()), p.second);
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_data, p.first, chunk));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	}else if(req.command() == EVIOSCLOCKID) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		// TODO: Does this setting affect already queued events in Linux?
		switch(req.input_clock()) {
		case CLOCK_REALTIME:
		case CLOCK_MONOTONIC:
			self->_clockId = req.input_clock();
			resp.set_error(managarm::fs::Errors::SUCCESS);
			break;
		default:
			assert(!"Clock is not supported in libevbackend");
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == EVIOCGABS(0)) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		if(logRequests)
			std::cout << "EVIOCGABS(" << req.input_type() << ")" << std::endl;

		assert(req.input_type() < self->_device->_absoluteSlots.size());
		auto slot = &self->_device->_absoluteSlots[req.input_type()];
		resp.set_input_value(slot->value);
		resp.set_input_min(slot->minimum);
		resp.set_input_max(slot->maximum);
		resp.set_input_fuzz(0);
		resp.set_input_flat(0);
		resp.set_input_resolution(1);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		throw std::runtime_error("Unknown ioctl() with ID " + std::to_string(req.command()));
	}
	COFIBER_RETURN();
}))


constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&File::read)
	.withWrite(&File::write)
	.withPoll(&File::poll)
	.withIoctl(&File::ioctl);

helix::UniqueLane File::serve(smarter::shared_ptr<File> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	protocols::fs::servePassthrough(std::move(local_lane), file,
			&fileOperations);
	return std::move(remote_lane);
}

File::File(EventDevice *device, bool non_block)
: _device{device}, _nonBlock{non_block}, _clockId{CLOCK_MONOTONIC} { }

// ----------------------------------------------------------------------------
// EventDevice implementation.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane p), ([device = std::move(device), lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_pt;
			helix::PushDescriptor push_page;
		
			auto file = smarter::make_shared<File>(device.get(),
					req.flags() & managarm::fs::OF_NONBLOCK);
			auto remote_lane = File::serve(std::move(file));
			
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FC_STATUS_PAGE);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_pt, remote_lane, kHelItemChain),
					helix::action(&push_page, device->statusPageMemory()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else{
			throw std::runtime_error("Invalid serveDevice request!");
		}
	}
}))

EventDevice::EventDevice()
: _currentSeq{1} {
	_statusPage.update(_currentSeq, 0);

	memset(_typeBits.data(), 0, _typeBits.size());
	memset(_keyBits.data(), 0, _keyBits.size());
	memset(_relBits.data(), 0, _relBits.size());
	memset(_absBits.data(), 0, _absBits.size());
	
	memset(_currentKeys.data(), 0, _currentKeys.size());
	memset(_absoluteSlots.data(), 0, _absoluteSlots.size() * sizeof(AbsoluteSlot));
}

void EventDevice::setAbsoluteDetails(int code, int minimum, int maximum) {
	assert(code < _absoluteSlots.size());
	_absoluteSlots[code].minimum = minimum;
	_absoluteSlots[code].maximum = maximum;
}

void EventDevice::enableEvent(int type, int code) {
	auto setBit = [] (uint8_t *array, size_t length, unsigned int bit) {
		assert(bit / 8 < length);
		array[bit / 8] |= (1 << (bit % 8));
	};
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
	auto getBit = [] (uint8_t *array, size_t length, unsigned int bit) {
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

	auto event = new Event(type, code, value);
	if(logCodes)
		std::cout << "Event type: " << type << ", code: " << code
				<< ", value: " << value << std::endl;
	_emitted.push_back(*event);
}

void EventDevice::notify() {
	_events.splice(_events.end(), _emitted);
	_currentSeq++;
	_statusPage.update(_currentSeq, EPOLLIN);
	_statusBell.ring();
}

} // namespace libevbackend

