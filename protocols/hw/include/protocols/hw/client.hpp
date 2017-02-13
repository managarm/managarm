
#ifndef LIBHW_CLIENT_HPP
#define LIBHW_CLIENT_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace protocols {
namespace hw {

enum IoType {
	kIoTypeNone = 0,
	kIoTypePort = 1,
	kIoTypeMemory = 2
};

struct BarInfo {
	IoType ioType;
	uintptr_t address;
	size_t length;
};

struct PciInfo {
	BarInfo barInfo[6];
};

struct Device {
	Device(helix::UniqueLane lane)
	:_lane(std::move(lane)) { };
	
	async::result<PciInfo> getPciInfo();
	async::result<helix::UniqueDescriptor> accessBar(int index);
	async::result<helix::UniqueDescriptor> accessIrq();
	
	async::result<uint32_t> loadPciSpace(size_t offset, unsigned int size);
	async::result<void> storePciSpace(size_t offset, unsigned int size, uint32_t word);

private:
	helix::UniqueLane _lane;
};

} } // namespace protocols::hw

#endif // LIBHW_CLIENT_HPP

