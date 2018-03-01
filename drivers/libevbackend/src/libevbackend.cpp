
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
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <libevbackend.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "fs.pb.h"
#include "libevbackend.hpp"

namespace libevbackend {

// ----------------------------------------------------------------------------
// File implementation.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<protocols::fs::ReadResult>,
File::read(std::shared_ptr<void> object, void *buffer, size_t max_size), ([=] {
	auto self = static_cast<File *>(object.get());

	if(self->_nonBlock && self->_device->_events.empty())
		COFIBER_RETURN(protocols::fs::Error::wouldBlock);

	while(self->_device->_events.empty())
		COFIBER_AWAIT self->_device->_statusBell.async_wait();
	
	size_t written = 0;
	while(!self->_device->_events.empty()
			&& written + sizeof(input_event) <= max_size) {
		auto event = &self->_device->_events.front();
		self->_device->_events.pop_front();

		input_event uev;
		memset(&uev, 0, sizeof(input_event));
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

async::result<void> File::write(std::shared_ptr<void>,
		const void *, size_t) {
	throw std::runtime_error("write not yet implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::PollResult>,
File::poll(std::shared_ptr<void> object, uint64_t past_seq), ([=] {
	auto self = static_cast<File *>(object.get());

	assert(past_seq <= self->_device->_currentSeq);
	while(self->_device->_currentSeq == past_seq)
		COFIBER_AWAIT self->_device->_statusBell.async_wait();
	
	COFIBER_RETURN(protocols::fs::PollResult(self->_device->_currentSeq, EPOLLIN,
			self->_device->_events.empty() ? 0 : EPOLLIN));
}))

COFIBER_ROUTINE(async::result<void>,
File::ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([object = std::move(object),
		req = std::move(req), conversation = std::move(conversation)] {
	auto self = static_cast<File *>(object.get());
	if(req.command() == EVIOCGBIT(0, 0)) {
		assert(req.size());
		std::cout << "EVIOCGBIT()" << std::endl;

		helix::SendBuffer send_resp;
		helix::SendBuffer send_data;
		managarm::fs::SvrResponse resp;

		std::array<uint8_t, 4> bits;
		memset(bits.data(), 0, bits.size());
		bits[0] |= (1 << EV_REL);

		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto chunk = std::min(size_t(req.size()), bits.size());
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_data, bits.data(), chunk));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	}else if(req.command() == EVIOCGBIT(1, 0)) {
		assert(req.size());
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

helix::UniqueLane File::serve(std::shared_ptr<File> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	protocols::fs::servePassthrough(std::move(local_lane), file,
			&fileOperations);
	return std::move(remote_lane);
}

File::File(EventDevice *device, bool non_block)
: _device{device}, _nonBlock{non_block} { }

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
			helix::PushDescriptor push_node;
		
			auto file = std::make_shared<File>(device.get(),
					req.flags() & managarm::fs::OF_NONBLOCK);
			auto remote_lane = File::serve(std::move(file));
			
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid serveDevice request!");
		}
	}
}))

EventDevice::EventDevice()
: _currentSeq{1} {
	memset(_typeBits.data(), 0, _typeBits.size());
	memset(_keyBits.data(), 0, _keyBits.size());
	memset(_relBits.data(), 0, _relBits.size());
}

void EventDevice::enableEvent(int type, int code) {
	auto setBit = [] (uint8_t *array, size_t length, int bit) {
		assert(bit / 8 < length);
		array[bit / 8] |= (1 << (bit % 8));
	};

	if(type == EV_KEY) {
		setBit(_keyBits.data(), _keyBits.size(), code);
	}else if(type == EV_REL) {
		setBit(_relBits.data(), _relBits.size(), code);
	}else{
		throw std::runtime_error("Unexpected event type");
	}
	setBit(_typeBits.data(), _typeBits.size(), type);
}

void EventDevice::emitEvent(int type, int code, int value) {
	auto event = new Event(type, code, value);
	_emitted.push_back(*event);
	_currentSeq++;
	_statusBell.ring();
}

void EventDevice::notify() {
	_events.splice(_events.begin(), _emitted);
	_currentSeq++;
	_statusBell.ring();
}

} // namespace libevbackend

