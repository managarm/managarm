#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include <unordered_set>

#include "gfx.hpp"

std::unordered_set<int64_t> gpuMap;
async::oneshot_event pchRead;
std::optional<uint16_t> pch_dev;

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::result<void> setupDevice(mbus::Entity entity) {
	protocols::hw::Device dev(co_await entity.bind());

	co_await pchRead.wait();

	auto gfx = std::make_shared<GfxDevice>(std::move(dev), *pch_dev);
	auto config = co_await gfx->initialize();

	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"drvcore.mbus-parent", mbus::StringItem{std::to_string(entity.getId())}},
		{"unix.subsystem", mbus::StringItem{"drm"}},
		{"unix.devname", mbus::StringItem{"dri/card"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		drm_core::serveDrmDevice(gfx, std::move(local_lane));

		co_return std::move(remote_lane);
	});

	co_await config->waitForCompletion();
	co_await root.createObject("gfx_intel_lil", descriptor, std::move(handler));
	gpuMap.insert(entity.getId());
}

std::set<std::string> supported_pci_devices{
	"3184",
	"3185",
	"3E9B",
	"5916",
	"5917",
};

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	auto base_entity = co_await mbus::Instance::global().getEntity(base_id);

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "06"),
		mbus::EqualsFilter("pci-subclass", "01"),
		mbus::EqualsFilter("pci-vendor", "8086"),
		mbus::EqualsFilter("pci-bus", "00"),
		mbus::EqualsFilter("pci-slot", "1f"),
	});

	bool pch_discovered;
	auto handler = mbus::ObserverHandler{}
	.withAttach([&pch_discovered] (mbus::Entity, mbus::Properties properties) {
		auto device_str = std::get_if<mbus::StringItem>(&properties["pci-device"]);

		if(!pch_discovered) {
			pch_dev = std::stoi(device_str->value, 0, 16);
			pch_discovered = true;
			pchRead.raise();
		}
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));

	// Do not bind to devices that are already bound to this driver.
	if(gpuMap.find(base_entity.getId()) != gpuMap.end())
		co_return protocols::svrctl::Error::success;

	auto properties = co_await base_entity.getProperties();
	auto vendor_str = std::get_if<mbus::StringItem>(&properties["pci-vendor"]);
	auto device_str = std::get_if<mbus::StringItem>(&properties["pci-device"]);

	// Make sure that we only bind to supported devices.
	if(!vendor_str || vendor_str->value != "8086")
		co_return protocols::svrctl::Error::deviceNotSupported;

	auto class_str = std::get_if<mbus::StringItem>(&properties["pci-class"]);
	auto subclass_str = std::get_if<mbus::StringItem>(&properties["pci-subclass"]);

	if(!class_str || !subclass_str)
		co_return protocols::svrctl::Error::deviceNotSupported;

	if(class_str->value == "03" && subclass_str->value == "00") {
		if(!device_str || !supported_pci_devices.contains(device_str->value))
			co_return protocols::svrctl::Error::deviceNotSupported;

		co_await setupDevice(base_entity);
	}

	co_return protocols::svrctl::Error::success;
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "gfx/intel-lil: starting Intel (lil) graphics driver" << std::endl;

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}
