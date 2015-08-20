
#include "../kernel.hpp"

namespace thor {

void initializeTheSystem() {
	initializeLocalApic();
	setupLegacyPic();
}

void controlArch(int interface, const void *input, void *output) {
	switch(interface) {
	case kThorIfBootSecondary: {
		const uint32_t *apic_id = (const uint32_t *)input;
		bootSecondary(*apic_id);
	} break;	
	default:
		ASSERT(!"Illegal interface");
	}
}

} // namespace thor

