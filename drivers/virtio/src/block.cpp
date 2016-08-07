
#include <stdlib.h>
#include "block.hpp"

extern helx::EventHub eventHub;

namespace virtio {
namespace block {

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

UserRequest::UserRequest(uint64_t sector, void *buffer, size_t num_sectors,
		frigg::CallbackPtr<void()> callback)
: sector(sector), buffer(buffer), numSectors(num_sectors), callback(callback),
		numSubmitted(0), sectorsRead(0) { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device()
: libfs::BlockDevice(512), requestQueue(*this, 0) { }

void Device::doInitialize() {
	// perform device specific setup
	requestQueue.setupQueue();
	userRequestPtrs.resize(requestQueue.getSize());
	virtRequestBuffer = (VirtRequest *)malloc(requestQueue.getSize() * sizeof(VirtRequest));
	statusBuffer = (uint8_t *)malloc(requestQueue.getSize());

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer % sizeof(VirtRequest) == 0);
	
	// setup an interrupt for the device
	interrupt.wait(eventHub, CALLBACK_MEMBER(this, &Device::onInterrupt));

	libfs::runDevice(eventHub, this);
}

void Device::readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) {
	// natural alignment makes sure a sector does not cross a page boundary
	assert(((uintptr_t)buffer % 512) == 0);

//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	UserRequest *user_request = new UserRequest(sector, buffer, num_sectors, callback);
//	FIXME: is this assertion still nesecarry?
//	assert(pendingRequests.empty());
	if(!requestIsReady(user_request)) {
		pendingRequests.push(user_request);
	}else{
		submitRequest(user_request);
	}
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
		user_request->callback();
		delete user_request;
		completeStack.pop_back();
	}
}

void Device::onInterrupt(HelError error) {
	HEL_CHECK(error);

	readIsr();
	requestQueue.processInterrupt();

	interrupt.wait(eventHub, CALLBACK_MEMBER(this, &Device::onInterrupt));
}

bool Device::requestIsReady(UserRequest *user_request) {
	return requestQueue.numLockable() > 2;
}

void Device::submitRequest(UserRequest *user_request) {
	assert(user_request->numSubmitted == 0);
	assert(user_request->sectorsRead < user_request->numSectors);

	// setup the actual request header
	size_t header_index = requestQueue.lockDescriptor();

	VirtRequest *header = &virtRequestBuffer[header_index];
	header->type = VIRTIO_BLK_T_IN;
	header->reserved = 0;
	header->sector = user_request->sector + user_request->sectorsRead;

	// setup a descriptor for the request header
	uintptr_t header_physical;
	HEL_CHECK(helPointerPhysical(header, &header_physical));
	
	VirtDescriptor *header_desc = requestQueue.accessDescriptor(header_index);
	header_desc->address = header_physical;
	header_desc->length = sizeof(VirtRequest);
	header_desc->flags = 0;

	size_t num_lockable = requestQueue.numLockable();
	assert(num_lockable > 2);
	size_t max_data_chain = num_lockable - 2;

	// setup descriptors for the transfered data
	VirtDescriptor *chain_desc = header_desc;
	for(size_t i = 0; i < max_data_chain; i++) {
		size_t offset = user_request->sectorsRead + user_request->numSubmitted;
		if(offset == user_request->numSectors)
			break;
		assert(offset < user_request->numSectors);
		
		uintptr_t data_physical;
		HEL_CHECK(helPointerPhysical((char *)user_request->buffer + offset * 512,
				&data_physical));

		size_t data_index = requestQueue.lockDescriptor();
		VirtDescriptor *data_desc = requestQueue.accessDescriptor(data_index);
		data_desc->address = data_physical;
		data_desc->length = 512;
		data_desc->flags = VIRTQ_DESC_F_WRITE;

		chain_desc->flags |= VIRTQ_DESC_F_NEXT;
		chain_desc->next = data_index;

		user_request->numSubmitted++;
		chain_desc = data_desc;
	}
//	printf("Submitting %lu data descriptors\n", user_request->numSubmitted);

	// setup a descriptor for the status byte	
	uintptr_t status_physical;
	HEL_CHECK(helPointerPhysical(&statusBuffer[header_index], &status_physical));
	
	size_t status_index = requestQueue.lockDescriptor();
	VirtDescriptor *status_desc = requestQueue.accessDescriptor(status_index);
	status_desc->address = status_physical;
	status_desc->length = 1;
	status_desc->flags = VIRTQ_DESC_F_WRITE;

	chain_desc->flags |= VIRTQ_DESC_F_NEXT;
	chain_desc->next = status_index;

	// submit the request to the device
	assert(!userRequestPtrs[header_index]);
	userRequestPtrs[header_index] = user_request;
	requestQueue.postDescriptor(header_index);
	requestQueue.notifyDevice();
}

} } // namespace virtio::block
