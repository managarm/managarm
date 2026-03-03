#include <core/id-allocator.hpp>
#include <protocols/mbus/client.hpp>

#include "../drvcore.hpp"

namespace graphics_subsystem {

namespace {

drvcore::ClassSubsystem *graphicsSubsystem;

struct Framebuffer final : drvcore::ClassDevice {
	Framebuffer(drvcore::ClassSubsystem *subsystem, size_t num)
	: drvcore::ClassDevice{subsystem, nullptr, std::format("freestanding-fb{}", num), nullptr} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "graphics");
	}

	std::optional<std::string> getClassPath() override {
		return "graphics";
	};
};

} // namespace

async::detached run() {
	graphicsSubsystem = new drvcore::ClassSubsystem{"graphics"};

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "framebuffer"},
		mbus_ng::EqualsFilter{"unix.subsystem", "graphics"},
	}};

	size_t num = 0;

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			auto fb = std::make_shared<Framebuffer>(graphicsSubsystem, num++);

			drvcore::installDevice(fb);

			std::println("posix: Installed freestanding framebuffer {} (mbus ID {})", fb->name(), entity.id());
			drvcore::registerMbusDevice(entity.id(), std::move(fb));
		}
	}

	co_return;
}

} // namespace graphics_subsystem
