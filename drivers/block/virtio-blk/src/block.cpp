
#include <async/basic.hpp>
#include <frg/scope_exit.hpp>
#include <helix/clock.hpp>
#include <stdlib.h>
#include <iostream>
#include <list>

#include "block.hpp"

namespace block {
namespace virtio {

static bool logInitiateRetire = false;

namespace {

uint64_t currentNs() {
	return helix::getClock();
}

void emitRequestTrace(UserRequest *request) {
	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkRequest,
		blockfs::ostAttrTime(request->completeTs - request->enqueueTs),
		blockfs::ostAttrNumBytes(request->view.size()),
		blockfs::ostAttrIsWrite(request->write),
		blockfs::ostAttrTimeSetup(request->setupTime),
		blockfs::ostAttrTimeObtain(request->obtainTime),
		blockfs::ostAttrTimeDevice(request->completeTs - request->submitTs)
	);
}

} // anonymous namespace

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
	// Without VIRTIO_BLK_F_FLUSH, qemu disables its write cache and fdatasync()s on every write.
	// Negotiate it to get writeback caching on the host side.
	if(_transport->checkDeviceFeature(VIRTIO_BLK_F_FLUSH)) {
		_transport->acknowledgeDriverFeature(VIRTIO_BLK_F_FLUSH);
		_hasFlush = true;
	}
	_transport->finalizeFeatures();
	_transport->claimQueues(1);
	_requestQueue = co_await _transport->setupQueue(0);
	async::detach(_requestQueue->serviceQueue());

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

	protocols::ostrace::Timer timer;

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
		request.enqueueTs = currentNs();
		co_await _issueRequest(&request);
	}

	for(auto &request : requests) {
		co_await request.event.wait();
		if(*request.status != VIRTIO_BLK_S_OK) {
			std::cout << "virtio: Device signaled an error for request at sector "
					<< request.sector << ", status "
					<< static_cast<unsigned int>(*request.status) << std::endl;
			abort();
		}
		emitRequestTrace(&request);
	}

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkReadSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
}

async::result<void> Device::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("writeSectors(%lu, %lu)\n", sector, num_sectors);

	protocols::ostrace::Timer timer;

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
		request.enqueueTs = currentNs();
		co_await _issueRequest(&request);
	}

	for(auto &request : requests) {
		co_await request.event.wait();
		if(*request.status != VIRTIO_BLK_S_OK) {
			std::cout << "virtio: Device signaled an error for request at sector "
					<< request.sector << ", status "
					<< static_cast<unsigned int>(*request.status) << std::endl;
			abort();
		}
		emitRequestTrace(&request);
	}

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkWriteSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
}

async::result<void> Device::flush() {
	protocols::ostrace::Timer timer;
	uint64_t setupTime = 0;
	uint64_t obtainTime = 0;
	uint64_t deviceTime = 0;
	frg::scope_exit evtOnExit{[&] {
		blockfs::ostContext.emit(
			blockfs::ostEvtVirtioBlkFlush,
			blockfs::ostAttrTime(timer.elapsed()),
			blockfs::ostAttrTimeSetup(setupTime),
			blockfs::ostAttrTimeObtain(obtainTime),
			blockfs::ostAttrTimeDevice(deviceTime)
		);
	}};

	// Devices without a flush command still emit the event, with a zero device time.
	if(!_hasFlush)
		co_return;

	UserRequest request{false, 0, {}, pagePool};

	std::array<virtio_core::Handle, 2> handles;
	setupTime += timer.split();
	co_await _requestQueue->obtainDescriptors(handles);
	obtainTime = timer.split();
	handles[0].setupLink(handles[1]);

	request.header->type = VIRTIO_BLK_T_FLUSH;
	request.header->reserved = 0;
	request.header->sector = 0;

	co_await handles[0].setupBuffer(virtio_core::hostToDevice, request.header.view_buffer());
	co_await handles[1].setupBuffer(virtio_core::deviceToHost, request.status.view_buffer());

	setupTime += timer.split();
	request.submitTs = currentNs();

	_requestQueue->postDescriptor(handles.front(), &request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		request->completeTs = currentNs();
		request->event.raise();
	});
	_requestQueue->notify();

	co_await request.event.wait();
	deviceTime = request.completeTs - request.submitTs;
	if(*request.status != VIRTIO_BLK_S_OK) {
		std::cout << "virtio: Device signaled an error for flush, status "
				<< static_cast<unsigned int>(*request.status) << std::endl;
		abort();
	}
}

async::result<size_t> Device::getSize() {
	co_return _size * 512;
}

async::result<void> Device::_issueRequest(UserRequest *request) {
	assert(request->view.size());

	protocols::ostrace::Timer setupTimer;

	// Split the view into DMA-contiguous chunks (and not into individual sectors)
	// since setupBuffer() only requires contiguity in DMA space per descriptor.
	auto chunks = co_await _requestQueue->splitContiguous(request->view);

	// Acquire all descriptors of the chain at once to avoid potential deadlocks.
	std::vector<virtio_core::Handle> handles(2 + chunks.size());
	request->setupTime += setupTimer.split();
	co_await _requestQueue->obtainDescriptors(handles);
	request->obtainTime = setupTimer.split();

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
	for(size_t i = 0; i < chunks.size(); i++) {
		if(request->write) {
			handles[1 + i].setupBuffer(virtio_core::hostToDevice, chunks[i]);
		}else{
			handles[1 + i].setupBuffer(virtio_core::deviceToHost, chunks[i]);
		}
	}

	if(logInitiateRetire)
		std::cout << "Submitting " << chunks.size() << " data descriptors" << std::endl;

	// Setup a descriptor for the status byte.
	co_await handles.back().setupBuffer(
	    virtio_core::deviceToHost,
	    request->status.view_buffer()
	);

	// Stamp the submission before posting: the completion callback may run
	// as soon as the descriptor is posted.
	request->setupTime += setupTimer.split();
	request->submitTs = currentNs();

	// Submit the request to the device
	_requestQueue->postDescriptor(handles.front(), request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		if(logInitiateRetire)
			std::cout << "Retiring request of " << request->view.size()
					<< " bytes" << std::endl;
		request->completeTs = currentNs();
		request->event.raise();
	});
	_requestQueue->notify();
}

} } // namespace block::virtio
