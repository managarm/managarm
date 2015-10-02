
#include <frigg/cxx-support.hpp>
#include <hel.h>
#include <hel-syscalls.h>

#include "common.hpp"
#include "device.hpp"

// --------------------------------------------------------
// Device
// --------------------------------------------------------

void Device::write(const void *buffer, size_t length) {
	assert(!"Illegal operation for this device");
}
void Device::read(void *buffer, size_t max_length, size_t &actual_length) {
	assert(!"Illegal operation for this device");
}

// --------------------------------------------------------
// DeviceAllocator
// --------------------------------------------------------

DeviceAllocator::DeviceAllocator()
: majorTable(*allocator) { }

unsigned int DeviceAllocator::allocateSlot(unsigned int major, StdSharedPtr<Device> device) {
	unsigned int index = majorTable[major].minorTable.size();
	majorTable[major].minorTable.push(frigg::move(device));
	return index;
}

unsigned int DeviceAllocator::accessGroup(frigg::StringView group_name) {
	size_t index;
	for(index = 0; index < majorTable.size(); index++)
		if(majorTable[index].groupName == group_name)
			return index;

	majorTable.push(SecondaryTable(frigg::String<Allocator>(*allocator, group_name)));
	return index;
}

void DeviceAllocator::allocateDevice(frigg::StringView group_name,
		StdSharedPtr<Device> device, unsigned int &major, unsigned int &minor) {
	major = accessGroup(group_name);
	minor = allocateSlot(major, frigg::move(device));
}

StdUnsafePtr<Device> DeviceAllocator::getDevice(unsigned int major, unsigned int minor) {
	if(major >= majorTable.size())
		return StdUnsafePtr<Device>();
	if(minor >= majorTable[major].minorTable.size())
		return StdUnsafePtr<Device>();
	return majorTable[major].minorTable[minor];
}

// --------------------------------------------------------
// DeviceAllocator::SecondaryTable
// --------------------------------------------------------

DeviceAllocator::SecondaryTable::SecondaryTable(frigg::String<Allocator> group_name)
: groupName(frigg::move(group_name)), minorTable(*allocator) { }

// --------------------------------------------------------
// KernelOutDevice
// --------------------------------------------------------

void KernelOutDevice::write(const void *buffer, size_t length) {
	HEL_CHECK(helLog((const char *)buffer, length));
}

