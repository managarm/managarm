#include "quirks.hpp"
#include "quirks/wacom.hpp"
#include "spec.hpp"

#include <format>

namespace {

constexpr bool logQuirks = false;

struct hidDescriptor {
	uint16_t idVendor;
	uint16_t idProduct;
	std::optional<uint16_t> usagePage;
	std::optional<uint16_t> usageId;
	void (*handler)(uint16_t usagePage, uint16_t usageId, Field &f);
	std::string desc;
};

// HID quirks that operate by modifying Fields parsed from report descriptors
std::array<struct hidDescriptor, 1> report_descriptor = {{
	{0x056a, 0x509c, pages::digitizers, usage::digitizers::contactIdentifier, quirks::wacom::touchHidLimits, "Wacom touchscreen contact ID fix"},
}};

} // namespace

namespace quirks {

void processField(HidDevice *dev, uint16_t usagePage, uint16_t usageId, Field &f) {
	for(auto &e : report_descriptor) {
		auto [vendor, product] = dev->getDeviceId();

		if(e.idVendor != vendor || e.idProduct != product)
			continue;

		if(e.usagePage && e.usagePage.value() != usagePage)
			continue;

		if(e.usageId && e.usageId.value() != usageId)
			continue;

		if(logQuirks)
			std::cout << std::format("hid: matched HID quirk '{}' for device {:04x}:{:04x}\n", e.desc, vendor, product);

		e.handler(usagePage, usageId, f);
	}
}

} // namespace quirks
