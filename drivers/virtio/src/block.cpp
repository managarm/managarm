
#include <stdlib.h>
#include <iostream>
#include <helix/await.hpp>

#include "block.hpp"

namespace virtio {
namespace block {

static bool logInitiateRetire = false;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

UserRequest::UserRequest(uint64_t sector, void *buffer, size_t num_sectors)
: sector{sector}, buffer{buffer}, numSectors{num_sectors} { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device()
: blockfs::BlockDevice(512), _requestQueue(this, 0) { }

void Device::doInitialize() {
	// perform device specific setup
	_requestQueue.setupQueue();
	virtRequestBuffer = (VirtRequest *)malloc(_requestQueue.numDescriptors() * sizeof(VirtRequest));
	statusBuffer = (uint8_t *)malloc(_requestQueue.numDescriptors());

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer % sizeof(VirtRequest) == 0);
	
	// setup an interrupt for the device
	_processRequests();
	_processIrqs();

	blockfs::runDevice(this);
}

COFIBER_ROUTINE(async::result<void>, Device::readSectors(uint64_t sector,
		void *buffer, size_t num_sectors), ([=] {
	// natural alignment makes sure a sector does not cross a page boundary
	assert(((uintptr_t)buffer % 512) == 0);
//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue.numDescriptors() / 4;
	assert(max_sectors >= 1);

	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto request = new UserRequest(sector + progress, (char *)buffer + 512 * progress,
				std::min(num_sectors - progress, max_sectors));
		_pendingQueue.push(request);
		_pendingDoorbell.ring();
		COFIBER_AWAIT request->promise.async_get();
		delete request;
	}

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::no_future, Device::_processIrqs(), ([=] {
	uint64_t sequence;
	while(true) {
		helix::AwaitEvent await;
		auto &&submit = helix::submitAwaitEvent(interrupt, &await, sequence,
				helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(await.error());
		sequence = await.sequence();

		readIsr();
		HEL_CHECK(helAcknowledgeIrq(interrupt.getHandle(), 0, sequence));
		_requestQueue.processInterrupt();
	}
}))

COFIBER_ROUTINE(cofiber::no_future, Device::_processRequests(), ([=] {
	while(true) {
		if(_pendingQueue.empty()) {
			COFIBER_AWAIT _pendingDoorbell.async_wait();
			continue;
		}

		auto request = _pendingQueue.front();
		_pendingQueue.pop();

		// Setup the descriptor for the request header.
		auto header_handle = COFIBER_AWAIT _requestQueue.obtainDescriptor();

		VirtRequest *header = &virtRequestBuffer[header_handle.tableIndex()];
		header->type = VIRTIO_BLK_T_IN;
		header->reserved = 0;
		header->sector = request->sector;

		header_handle.setupBuffer(hostToDevice, arch::dma_buffer_view{nullptr,
				header, sizeof(VirtRequest)});
		
		// Setup descriptors for the transfered data.
		auto chain_handle = header_handle;
		for(size_t i = 0; i < request->numSectors; i++) {
			auto data_handle = COFIBER_AWAIT _requestQueue.obtainDescriptor();
			data_handle.setupBuffer(deviceToHost, arch::dma_buffer_view{nullptr,
					(char *)request->buffer + 512 * i, 512});
			chain_handle.setupLink(data_handle);
			chain_handle = data_handle;
		}

		if(logInitiateRetire)
			std::cout << "Submitting " << request->numSectors
					<< " data descriptors" << std::endl;

		// Setup a descriptor for the status byte.
		auto status_handle = COFIBER_AWAIT _requestQueue.obtainDescriptor();
		status_handle.setupBuffer(deviceToHost, arch::dma_buffer_view{nullptr,
				&statusBuffer[header_handle.tableIndex()], 1});
		chain_handle.setupLink(status_handle);

		// Submit the request to the device
		_requestQueue.postDescriptor(header_handle, request, [] (Request *base_request) {
			auto request = static_cast<UserRequest *>(base_request);
			if(logInitiateRetire)
				std::cout << "Retiring " << request->numSectors
						<< " data descriptors" << std::endl;
			request->promise.set_value();
		});
		_requestQueue.notifyDevice();
	}
}))

} } // namespace virtio::block
