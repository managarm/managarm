
#include <async/basic.hpp>
#include <stdlib.h>
#include <iostream>
#include <list>

#include "block.hpp"

namespace block {
namespace virtio {

static bool logInitiateRetire = false;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

// Poison the status byte by initializing it to 0xFF.
UserRequest::UserRequest(bool write_, uint64_t sector_, arch::dma_buffer_view view_,
		arch::dma_pool *pool)
: write{write_}, sector{sector_}, view{view_}, header{pool}, status{pool, uint8_t{0xFF}} { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id)
: blockfs::BlockDevice{512, parent_id, &transport->memoryPool_},
  _transport{std::move(transport)},
  _requestQueue{nullptr},
  _size{0} {}

async::result<void> Device::runDevice() {
	_transport->finalizeFeatures();
	_transport->claimQueues(1);
	_requestQueue = co_await _transport->setupQueue(0);

	auto size = static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[0]))
			| (static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[1])) << 32);
	std::cout << "virtio: Disk size: " << size << " sectors" << std::endl;
	_size = size;

	_transport->runDevice();

	blockfs::runDevice(this);
}

async::result<void> Device::readSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() >> sectorShift;

	// Issue all requests first, then wait for completion.
	// Note that the individual requests can be interleaved with other virtio-block requests.
	std::list<UserRequest> requests;
	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    progress << sectorShift, std::min(num_sectors - progress, max_sectors) << sectorShift
		);
		auto &request = requests.emplace_back(false, sector + progress, subview, pagePool);
		co_await _issueRequest(&request);
	}

	for(auto &request : requests)
		co_await request.event.wait();
}

async::result<void> Device::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("writeSectors(%lu, %lu)\n", sector, num_sectors);

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() >> sectorShift;

	// Issue all requests first, then wait for completion.
	// Note that the individual requests can be interleaved with other virtio-block requests.
	std::list<UserRequest> requests;
	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    progress << sectorShift, std::min(num_sectors - progress, max_sectors) << sectorShift
		);
		auto &request = requests.emplace_back(true, sector + progress, subview, pagePool);
		co_await _issueRequest(&request);
	}

	for(auto &request : requests)
		co_await request.event.wait();
}

async::result<size_t> Device::getSize() {
	co_return _size * 512;
}

async::result<void> Device::_issueRequest(UserRequest *request) {
	auto numSectors = request->view.size() >> sectorShift;
	assert(numSectors);

	// Acquire all descriptors of the chain at once to avoid potential deadlocks.
	std::vector<virtio_core::Handle> handles(2 + numSectors);
	co_await _requestQueue->obtainDescriptors(handles);

	for(size_t i = 1; i < handles.size(); i++)
		handles[i - 1].setupLink(handles[i]);

	// Setup the descriptor for the request header.
	if(request->write) {
		request->header->type = VIRTIO_BLK_T_OUT;
	}else{
		request->header->type = VIRTIO_BLK_T_IN;
	}
	request->header->reserved = 0;
	request->header->sector = request->sector;

	co_await handles.front().setupBuffer(virtio_core::hostToDevice, request->header.view_buffer());

	// Setup descriptors for the transfered data.
	for(size_t i = 0; i < numSectors; i++) {
		if(request->write) {
			co_await handles[1 + i].setupBuffer(virtio_core::hostToDevice,
					request->view.subview(i << sectorShift, sectorSize));
		}else{
			co_await handles[1 + i].setupBuffer(virtio_core::deviceToHost,
					request->view.subview(i << sectorShift, sectorSize));
		}
	}

	if(logInitiateRetire)
		std::cout << "Submitting " << numSectors << " data descriptors" << std::endl;

	// Setup a descriptor for the status byte.
	co_await handles.back().setupBuffer(
	    virtio_core::deviceToHost,
	    request->status.view_buffer()
	);

	// Submit the request to the device
	_requestQueue->postDescriptor(handles.front(), request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		if(logInitiateRetire)
			std::cout << "Retiring " << (request->view.size() / 512uz)
					<< " data descriptors" << std::endl;
		request->event.raise();
	});
	_requestQueue->notify();
}

} } // namespace block::virtio
