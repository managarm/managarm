#ifndef THOR_SRC_SYSTEM_FB
#define THOR_SRC_SYSTEM_FB

#include "../generic/address-space.hpp"

namespace thor {

struct FbInfo {
	uint64_t address;
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
	
	frigg::SharedPtr<Memory> memory;
};

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type, void *early_window);
void transitionBootFb();

} // namespace thor

#endif // THOR_SRC_SYSTEM_FB
