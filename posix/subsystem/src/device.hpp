
#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include <frigg/vector.hpp>
#include <frigg/string.hpp>

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device {
	virtual void write(const void *buffer, size_t length);
	virtual void read(void *buffer, size_t max_length, size_t &actual_length);
};

// --------------------------------------------------------
// DeviceAllocator
// --------------------------------------------------------

struct DeviceAllocator {
private:
	struct SecondaryTable {
		SecondaryTable(frigg::String<Allocator> group_name);

		frigg::String<Allocator> groupName;
		frigg::Vector<StdSharedPtr<Device>, Allocator> minorTable;
	};

public:
	DeviceAllocator();

	unsigned int allocateSlot(unsigned int major, StdSharedPtr<Device> device);
	
	unsigned int accessGroup(frigg::StringView group_name);

	void allocateDevice(frigg::StringView group_name,
			StdSharedPtr<Device> device, unsigned int &major, unsigned int &minor);

	StdUnsafePtr<Device> getDevice(unsigned int major, unsigned int minor);

private:
	frigg::Vector<SecondaryTable, Allocator> majorTable;
};


// --------------------------------------------------------
// KernelOutDevice
// --------------------------------------------------------

struct KernelOutDevice : public Device {
	// inherited from Device
	void write(const void *buffer, size_t length) override;
};

#endif // POSIX_SUBSYSTEM_DEVICE_HPP

