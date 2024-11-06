#include "../spec.hpp"
#include "wacom.hpp"

namespace quirks::wacom {

void touchHidLimits(uint16_t usagePage, uint16_t usageId, Field &f) {
	assert(usagePage == pages::digitizers && usageId == usage::digitizers::contactIdentifier);
	// on at least my Wacom touchscreen device (056a:509c), the HID report descriptor fails to
	// update the logical limits for Contact Identifier usages, which breaks multitouch input.

	// https://github.com/linuxwacom/wacom-hid-descriptors/blob/490ce1ccc1767531d269dac9f4d562425f22661a/Lenovo%20ThinkPad%20Yoga%20370/sysinfo.Mc7vuWOv8R/0003%3A056A%3A509F.0001.hid.txt

	// We fix this by overriding the limits for the affected usage without changing the global
	// parsing state.
	f.dataMin = 0;
	f.dataMax = UINT16_MAX;
}

} // namespace quirks::wacom
