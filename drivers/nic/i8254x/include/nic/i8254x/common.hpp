#pragma once

#include <async/basic.hpp>
#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>

constexpr bool logDebug = true;

struct Intel8254xNic : nic::Link {
public:
	Intel8254xNic(protocols::hw::Device device);

private:
	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;
};
