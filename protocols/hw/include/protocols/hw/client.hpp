
#ifndef LIBHW_CLIENT_HPP
#define LIBHW_CLIENT_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <vector>

namespace protocols {
namespace hw {

enum IoType {
	kIoTypeNone = 0,
	kIoTypePort = 1,
	kIoTypeMemory = 2
};

struct BarInfo {
	IoType ioType;
	IoType hostType;
	uintptr_t address;
	size_t length;
	ptrdiff_t offset;
};

struct Capability {
	unsigned int type;
};

struct PciInfo {
	BarInfo barInfo[6];
	std::vector<Capability> caps;
};

struct FbInfo {
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
};

struct Device {
	Device(helix::UniqueLane lane)
	:_lane(std::move(lane)) { };
	
	async::result<PciInfo> getPciInfo();
	async::result<helix::UniqueDescriptor> accessBar(int index);
	async::result<helix::UniqueDescriptor> accessIrq();

	async::result<void> claimDevice();
	async::result<void> enableBusIrq();
	async::result<void> enableBusmaster();

	async::result<uint32_t> loadPciSpace(size_t offset, unsigned int size);
	async::result<void> storePciSpace(size_t offset, unsigned int size, uint32_t word);
	async::result<uint32_t> loadPciCapability(unsigned int index, size_t offset, unsigned int size);
	
	async::result<FbInfo> getFbInfo();
	async::result<helix::UniqueDescriptor> accessFbMemory();

private:
	helix::UniqueLane _lane;
};

} } // namespace protocols::hw

#endif // LIBHW_CLIENT_HPP

