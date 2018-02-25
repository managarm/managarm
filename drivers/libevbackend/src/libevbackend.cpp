
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

// ----------------------------------------------------------------------------
// File implementation.
// ----------------------------------------------------------------------------

async::result<int64_t> File::seek(std::shared_ptr<void> object, int64_t offset) {
	throw std::runtime_error("seek not yet implemented");
}

async::result<size_t> File::read(std::shared_ptr<void> object, void *buffer, size_t length) {
	std::shared_ptr<File> self = std::static_pointer_cast<File>(object);
	
	auto req = new ReadRequest(buffer, length);
	self->_device->_requests.push_back(*req);
	auto value = req->promise.async_get();
	self->_device->_processEvents();
	return value;
}

async::result<void> File::write(std::shared_ptr<void> object,
		const void *buffer, size_t length) {
	throw std::runtime_error("write not yet implemented");
}

async::result<protocols::fs::AccessMemoryResult>
File::accessMemory(std::shared_ptr<void> object, uint64_t, size_t) {
	throw std::runtime_error("accessMemory not yet implemented");
}

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withSeekAbs(&File::seek)
	.withSeekRel(&File::seek)
	.withSeekEof(&File::seek)
	.withRead(&File::read)
	.withWrite(&File::write)
	.withAccessMemory(&File::accessMemory);

helix::UniqueLane File::serve(std::shared_ptr<File> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	protocols::fs::servePassthrough(std::move(local_lane), file,
			&fileOperations);
	return std::move(remote_lane);
}

File::File(EventDevice *device, bool non_block)
: _device{device} { }

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

