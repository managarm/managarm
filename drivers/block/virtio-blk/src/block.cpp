
#include <async/basic.hpp>
#include <stdlib.h>
#include <iostream>

#include "block.hpp"

namespace block {
namespace virtio {

static bool logInitiateRetire = false;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

UserRequest::UserRequest(bool write_, uint64_t sector_, arch::dma_buffer_view view_)
: write{write_}, sector{sector_}, view{view_} { }

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

	// perform device specific setup
	virtRequestBuffer = arch::dma_array<VirtRequest>{pagePool, _requestQueue->numDescriptors()};
	statusBuffer = arch::dma_array<uint8_t>{pagePool, _requestQueue->numDescriptors()};

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer.byte_data() % sizeof(VirtRequest) == 0);

	// setup an interrupt for the device
	_processRequests();

	blockfs::runDevice(this);
}

async::result<void> Device::readSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() / sectorSize;

	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    sectorSize * progress, std::min(num_sectors - progress, max_sectors) * sectorSize
		);
		auto request = new UserRequest(false, sector + progress, subview);
		_pendingQueue.push(request);
		_pendingDoorbell.raise();
		co_await request->event.wait();
		delete request;
	}
}

async::result<void> Device::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("writeSectors(%lu, %lu)\n", sector, num_sectors);

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() / sectorSize;

	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    sectorSize * progress, std::min(num_sectors - progress, max_sectors) * sectorSize
		);
		auto request = new UserRequest(true, sector + progress, subview);
		_pendingQueue.push(request);
		_pendingDoorbell.raise();
		co_await request->event.wait();
		delete request;
	}
}

async::result<size_t> Device::getSize() {
	co_return _size * 512;
}

async::detached Device::_processRequests() {
	while(true) {
		if(_pendingQueue.empty()) {
			co_await _pendingDoorbell.async_wait();
			continue;
		}

		auto request = _pendingQueue.front();
		_pendingQueue.pop();
		auto numSectors = request->view.size() / sectorSize;
		assert(numSectors);

		// Setup the descriptor for the request header.
		virtio_core::Chain chain;
		chain.append(co_await _requestQueue->obtainDescriptor());

		auto header = virtRequestBuffer.object_view(chain.front().tableIndex());
		if(request->write) {
			header->type = VIRTIO_BLK_T_OUT;
		}else{
			header->type = VIRTIO_BLK_T_IN;
		}
		header->reserved = 0;
		header->sector = request->sector;

		co_await chain.setupBuffer(virtio_core::hostToDevice, header.view_buffer());

		// Setup descriptors for the transfered data.
		for(size_t i = 0; i < numSectors; i++) {
			chain.append(co_await _requestQueue->obtainDescriptor());
			if(request->write) {
				co_await chain.setupBuffer(virtio_core::hostToDevice, request->view.subview(sectorSize * i, sectorSize));
			}else{
				co_await chain.setupBuffer(virtio_core::deviceToHost, request->view.subview(sectorSize * i, sectorSize));
			}
		}

		if(logInitiateRetire)
			std::cout << "Submitting " << numSectors
					<< " data descriptors" << std::endl;

		// Setup a descriptor for the status byte.
		chain.append(co_await _requestQueue->obtainDescriptor());
		co_await chain.setupBuffer(
		    virtio_core::deviceToHost,
		    statusBuffer.object_view(chain.front().tableIndex()).view_buffer()
		);

		// Submit the request to the device
		_requestQueue->postDescriptor(chain.front(), request,
				[] (virtio_core::Request *base_request) {
			auto request = static_cast<UserRequest *>(base_request);
			if(logInitiateRetire)
				std::cout << "Retiring " << (request->view.size() / 512uz)
						<< " data descriptors" << std::endl;
			request->event.raise();
		});
		_requestQueue->notify();
	}
}

} } // namespace block::virtio
