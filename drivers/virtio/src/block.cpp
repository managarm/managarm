
#include <stdlib.h>
#include <stdio.h>

#include <helix/await.hpp>

#include "block.hpp"

namespace virtio {
namespace block {

static bool logInitiateRetire = false;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

UserRequest::UserRequest(uint64_t sector, void *buffer, size_t num_sectors)
: sector(sector), buffer(buffer), numSectors(num_sectors),
		numSubmitted(0), sectorsRead(0) { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device()
: blockfs::BlockDevice(512), requestQueue(this, 0) { }

void Device::doInitialize() {
	// perform device specific setup
	requestQueue.setupQueue();
	userRequestPtrs.resize(requestQueue.numDescriptors());
	virtRequestBuffer = (VirtRequest *)malloc(requestQueue.numDescriptors() * sizeof(VirtRequest));
	statusBuffer = (uint8_t *)malloc(requestQueue.numDescriptors());

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer % sizeof(VirtRequest) == 0);
	
	// setup an interrupt for the device
	processIrqs();

	blockfs::runDevice(this);
}

async::result<void> Device::readSectors(uint64_t sector, void *buffer, size_t num_sectors) {
	// natural alignment makes sure a sector does not cross a page boundary
	assert(((uintptr_t)buffer % 512) == 0);

//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	UserRequest *user_request = new UserRequest(sector, buffer, num_sectors);
	auto future = user_request->promise.async_get();

//	FIXME: is this assertion still nesecarry?
//	assert(pendingRequests.empty());
	if(!requestIsReady(user_request)) {
		pendingRequests.push(user_request);
	}else{
		submitRequest(user_request);
	}

	return future;
}

void Device::retrieveDescriptor(size_t queue_index, size_t desc_index, size_t bytes_written) {
	assert(queue_index == 0);

	UserRequest *user_request = userRequestPtrs[desc_index];
	assert(user_request);
	userRequestPtrs[desc_index] = nullptr;

	// check the status byte
	assert(user_request->numSubmitted > 0);
	assert(statusBuffer[desc_index] == 0);

	user_request->sectorsRead += user_request->numSubmitted;
	user_request->numSubmitted = 0;
	
	if(logInitiateRetire)
		printf("Initiate %p\n", user_request);

	// re-submit the request if it is not complete yet
	if(user_request->sectorsRead < user_request->numSectors) {
		pendingRequests.push(user_request);
	}else{
		completeStack.push_back(user_request);
	}
}

void Device::afterRetrieve() {
	while(!pendingRequests.empty()) {
		UserRequest *user_request = pendingRequests.front();
		if(!requestIsReady(user_request))
			break;
		submitRequest(user_request);
		pendingRequests.pop();
	}

	while(!completeStack.empty()) {
		UserRequest *user_request = completeStack.back();
		user_request->promise.set_value();

		if(logInitiateRetire)
			printf("Retire %p\n", user_request);

		delete user_request;
		completeStack.pop_back();
	}
}

COFIBER_ROUTINE(cofiber::no_future, Device::processIrqs(), ([=] {
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
		requestQueue.processInterrupt();
	}
}))

bool Device::requestIsReady(UserRequest *user_request) {
	return requestQueue.numUnusedDescriptors() > 2;
}

COFIBER_ROUTINE(cofiber::no_future, Device::submitRequest(UserRequest *user_request), ([=] {
	assert(user_request->numSubmitted == 0);
	assert(user_request->sectorsRead < user_request->numSectors);

	// Setup the descriptor for the request header.
	auto header_handle = COFIBER_AWAIT requestQueue.obtainDescriptor();

	VirtRequest *header = &virtRequestBuffer[header_handle.tableIndex()];
	header->type = VIRTIO_BLK_T_IN;
	header->reserved = 0;
	header->sector = user_request->sector + user_request->sectorsRead;

	header_handle.setupBuffer(hostToDevice, header, sizeof(VirtRequest));

	// Setup descriptors for the transfered data.
	size_t num_lockable = requestQueue.numUnusedDescriptors();
	assert(num_lockable > 2);
	size_t max_data_chain = num_lockable - 2;

	auto chain_handle = header_handle;
	for(size_t i = 0; i < max_data_chain; i++) {
		size_t offset = user_request->sectorsRead + user_request->numSubmitted;
		if(offset == user_request->numSectors)
			break;
		assert(offset < user_request->numSectors);

		auto data_handle = COFIBER_AWAIT requestQueue.obtainDescriptor();
		data_handle.setupBuffer(deviceToHost, (char *)user_request->buffer + offset * 512, 512);
		chain_handle.setupLink(data_handle);

		user_request->numSubmitted++;
		chain_handle = data_handle;
	}
//	printf("Submitting %lu data descriptors\n", user_request->numSubmitted);

	// Setup a descriptor for the status byte.
	auto status_handle = COFIBER_AWAIT requestQueue.obtainDescriptor();
	status_handle.setupBuffer(deviceToHost, &statusBuffer[header_handle.tableIndex()], 1);
	chain_handle.setupLink(status_handle);

	// Submit the request to the device
	assert(!userRequestPtrs[header_handle.tableIndex()]);
	userRequestPtrs[header_handle.tableIndex()] = user_request;
	requestQueue.postDescriptor(header_handle);
	requestQueue.notifyDevice();
}))

} } // namespace virtio::block
