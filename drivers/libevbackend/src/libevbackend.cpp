
#include <algorithm>
#include <deque>
#include <iostream>

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

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

async::result<int64_t> EventDevice::seek(std::shared_ptr<void> object, int64_t offset) {
	throw std::runtime_error("seek not yet implemented");
}

async::result<size_t> EventDevice::read(std::shared_ptr<void> object, void *buffer, size_t length) {
	std::shared_ptr<EventDevice> device = std::static_pointer_cast<EventDevice>(object);
	
	auto req = new ReadRequest(buffer, length);
	device->_requests.push_back(*req);
	auto value = req->promise.async_get();
	device->_processEvents();
	return value;
}

async::result<void> EventDevice::write(std::shared_ptr<void> object, const void *buffer, size_t length) {
	throw std::runtime_error("write not yet implemented");
}

async::result<protocols::fs::AccessMemoryResult> EventDevice::accessMemory(std::shared_ptr<void> object, uint64_t, size_t) {
	throw std::runtime_error("accessMemory not yet implemented");
}

constexpr protocols::fs::FileOperations fileOperations {
	&EventDevice::seek,
	&EventDevice::seek,
	&EventDevice::seek,
	&EventDevice::read,
	&EventDevice::write,
	&EventDevice::accessMemory
};

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
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::servePassthrough(std::move(local_lane), device,
					&fileOperations);

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
		
void EventDevice::_processEvents() {
	while(!_requests.empty() && !_events.empty()) {
		auto req = &_requests.front();
		auto event = &_events.front();
	
		// TODO: fill in timeval 
		input_event data;
		data.type = event->type;	
		data.code = event->code;
		data.value = event->value;

		assert(req->maxLength == sizeof(input_event));
		memcpy(req->buffer, &data, sizeof(input_event));	
		req->promise.set_value(sizeof(input_event));
		
		_requests.pop_front();
		_events.pop_front();
		delete req;
		delete event;
	}
}

void EventDevice::emitEvent(int type, int code, int value) {
	auto event = new Event(type, code, value);
	_events.push_back(*event);
	_processEvents();
}

} // namespace libevbackend

