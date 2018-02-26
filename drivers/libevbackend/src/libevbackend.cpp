
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

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&File::read)
	.withWrite(&File::write)
	.withPoll(&File::poll);

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
: _currentSeq{1} { }

void EventDevice::emitEvent(int type, int code, int value) {
	auto event = new Event(type, code, value);
	_events.push_back(*event);
	_currentSeq++;
	_statusBell.ring();
}

} // namespace libevbackend

