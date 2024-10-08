#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include <unordered_set>

#include "gfx.hpp"

std::unordered_set<int64_t> gpuMap;
std::optional<uint16_t> pch_dev;

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::result<void> setupDevice(mbus_ng::Entity entity) {
	protocols::hw::Device dev((co_await entity.getRemoteLane()).unwrap());

	auto gfx = std::make_shared<GfxDevice>(std::move(dev), *pch_dev);
	auto config = co_await gfx->initialize();

	mbus_ng::Properties descriptor{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity.id())}},
		{"unix.subsystem", mbus_ng::StringItem{"drm"}},
		{"unix.devname", mbus_ng::StringItem{"dri/card"}}
	};

	co_await config->waitForCompletion();

	auto gpu_entity = (co_await mbus_ng::Instance::global().createEntity("gfx_intel_lil", descriptor)).unwrap();

	[] (auto gfx, mbus_ng::EntityManager gpu_entity) -> async::detached {
		while(true) {
			auto [local_lane, remote_lane] = helix::createStream();
			// If this fails, too bad!
			(void)(co_await gpu_entity.serveRemoteLane(std::move(remote_lane)));
			drm_core::serveDrmDevice(gfx, std::move(local_lane));
		}
	}(std::move(gfx), std::move(gpu_entity));

	gpuMap.insert(entity.id());
}

std::set<std::string> supported_pci_devices{
	"3184",
	"3185",
	"3E9B",
	"5916",
	"5917",
};

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	auto base_entity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter("pci-class", "06"),
		mbus_ng::EqualsFilter("pci-subclass", "01"),
		mbus_ng::EqualsFilter("pci-vendor", "8086"),
		mbus_ng::EqualsFilter("pci-bus", "00"),
		mbus_ng::EqualsFilter("pci-slot", "1f"),
	});

	bool pch_discovered = false;

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while(!pch_discovered) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for(auto &event : events) {
			if(event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto device_str = std::get_if<mbus_ng::StringItem>(&event.properties["pci-device"]);

			pch_dev = std::stoi(device_str->value, 0, 16);
			pch_discovered = true;
		}
	}

	// Do not bind to devices that are already bound to this driver.
	if(gpuMap.find(base_entity.id()) != gpuMap.end())
		co_return protocols::svrctl::Error::success;

	auto properties = (co_await base_entity.getProperties()).unwrap();
	auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
	auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]);

	// Make sure that we only bind to supported devices.
	if(!vendor_str || vendor_str->value != "8086")
		co_return protocols::svrctl::Error::deviceNotSupported;

	auto class_str = std::get_if<mbus_ng::StringItem>(&properties["pci-class"]);
	auto subclass_str = std::get_if<mbus_ng::StringItem>(&properties["pci-subclass"]);

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
