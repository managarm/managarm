#include <libevbackend.hpp>
#include <memory>
#include <print>

#include "elantech.hpp"

async::result<std::expected<ElantechTouchpadDevice::ProbeData, Ps2Error>>
ElantechTouchpadDevice::probe(Controller::Port *port) {
	if (auto r = co_await port->transferByte(0xF6); !r || *r != 0xFA)
		co_return std::unexpected{Ps2Error::invalid};
	if (auto r = co_await port->transferByte(0xF5); !r || *r != 0xFA)
		co_return std::unexpected{Ps2Error::invalid};
	if (auto r = co_await port->transferByte(0xE6); !r || *r != 0xFA)
		co_return std::unexpected{Ps2Error::invalid};
	if (auto r = co_await port->transferByte(0xE6); !r || *r != 0xFA)
		co_return std::unexpected{Ps2Error::invalid};
	if (auto r = co_await port->transferByte(0xE6); !r || *r != 0xFA)
		co_return std::unexpected{Ps2Error::invalid};

	auto mouse = Controller::MouseDevice(port);

	auto status = co_await mouse.submitCommand(device_cmd::GetStatus{});
	if (!status)
		co_return std::unexpected{Ps2Error::invalid};

	auto magic = status.unwrap();
	if (magic[0] == 0x3C && magic[1] == 0x03 && (magic[2] == 0xC8 || magic[2] == 0x00)) {
		auto sliced = co_await port->submitCommand(
		    device_cmd::SlicedCommand{}, std::to_underlying(Query::FwVersion)
		);
		if (!sliced)
			co_return std::unexpected{Ps2Error::invalid};

		auto versionRead = co_await mouse.submitCommand(device_cmd::GetStatus{});
		if (!versionRead)
			co_return std::unexpected{Ps2Error::invalid};
		auto fw_version = versionRead.unwrap();

		if ((fw_version[0] & 0x0F) >= 0x06 && (fw_version[1] & 0xAF) == 0x0F && fw_version[2] < 40) {
			uint32_t fw = uint32_t{fw_version[0]} << 16 | uint32_t{fw_version[1]} << 8
			              | uint32_t{fw_version[2]};
			uint8_t hwVersion = 0;

			if (fw < 0x020030 || fw == 0x020600)
				hwVersion = 1;
			else if ((fw_version[0] & 0x0F) == 2 || (fw_version[0] & 0x0F) == 4)
				hwVersion = 2;
			else if ((fw_version[0] & 0x0F) == 5)
				hwVersion = 3;
			else if ((fw_version[0] & 0x0F) >= 6)
				hwVersion = 4;
			else
				co_return std::unexpected{Ps2Error::invalid};

			if (hwVersion != 4) {
				std::println("ps2-hid: unsupported Elantech touchpad v{}, skipping", hwVersion);
				co_return std::unexpected{Ps2Error::invalid};
			}

			co_return ProbeData{fw, hwVersion};
		}
	}

	co_return std::unexpected{Ps2Error::invalid};
}

async::result<void> ElantechTouchpadDevice::run() {
	auto res0 = co_await mouse_.submitCommand(device_cmd::Reset{});
	assert(res0);

	auto caps = co_await submitQuery(Query::Capabilities);
	assert(caps);

	auto samples = co_await submitQuery(Query::Sample);
	assert(samples);

	auto resolution = co_await submitQuery(Query::Resolution);
	assert(resolution);

	auto convertResolution = [](uint8_t val) {
		return (val * 10 + 790) * 10 / 254;
	};

	auto x_res = convertResolution(resolution.value()[1] & 0x0F);
	auto y_res = convertResolution(resolution.value()[1] >> 4);

	auto fwid = (co_await submitQuery(Query::FwId)).value();

	params.x_max = (0x0f & fwid[0]) << 8 | fwid[1];
	params.y_max = (0xf0 & fwid[0]) << 4 | fwid[2];
	params.traces = caps.value()[1];
	params.width = params.x_max / (params.traces - 1);

	// enable absolute mode
	auto setAbsoluteErr = co_await writeRegister(0x07, 0x01);
	assert(!setAbsoluteErr);

	evDev_ = std::make_shared<libevbackend::EventDevice>("ETPS/2 Elantech Touchpad", BUS_I8042, 0x0002, 0x000E);

	evDev_->enableEvent(EV_KEY, BTN_LEFT);
	evDev_->enableEvent(EV_KEY, BTN_RIGHT);
	evDev_->enableEvent(EV_KEY, BTN_MIDDLE);
	evDev_->enableEvent(EV_KEY, BTN_TOOL_FINGER);
	evDev_->enableEvent(EV_KEY, BTN_TOOL_DOUBLETAP);
	evDev_->enableEvent(EV_KEY, BTN_TOOL_TRIPLETAP);
	evDev_->enableEvent(EV_KEY, BTN_TOOL_QUADTAP);
	evDev_->enableEvent(EV_KEY, BTN_TOOL_QUINTTAP);
	evDev_->enableEvent(EV_KEY, BTN_TOUCH);

	evDev_->enableEvent(EV_ABS, ABS_X);
	evDev_->enableEvent(EV_ABS, ABS_Y);
	evDev_->enableEvent(EV_ABS, ABS_MT_SLOT);
	evDev_->enableEvent(EV_ABS, ABS_MT_TRACKING_ID);
	evDev_->enableEvent(EV_ABS, ABS_MT_POSITION_X);
	evDev_->enableEvent(EV_ABS, ABS_MT_POSITION_Y);
	evDev_->enableEvent(EV_ABS, ABS_PRESSURE);
	evDev_->enableEvent(EV_ABS, ABS_MT_PRESSURE);
	evDev_->enableEvent(EV_ABS, ABS_MT_TOUCH_MAJOR);
	evDev_->setAbsoluteDetails(ABS_X, 0, (fwid[0] & 0x0F) << 8 | fwid[1]);
	evDev_->setAbsoluteDetails(ABS_Y, 0, (fwid[0] & 0xF0) << 4 | fwid[2]);
	evDev_->setResolution(ABS_X, x_res);
	evDev_->setResolution(ABS_Y, y_res);
	evDev_->setAbsoluteDetails(ABS_MT_SLOT, 0, 4);
	evDev_->setAbsoluteDetails(ABS_MT_TRACKING_ID, 0, UINT16_MAX);
	evDev_->setAbsoluteDetails(ABS_MT_POSITION_X, 0, (fwid[0] & 0x0F) << 8 | fwid[1]);
	evDev_->setAbsoluteDetails(ABS_MT_POSITION_Y, 0, (fwid[0] & 0xF0) << 4 | fwid[2]);
	evDev_->setResolution(ABS_MT_POSITION_X, x_res);
	evDev_->setResolution(ABS_MT_POSITION_Y, y_res);
	evDev_->setAbsoluteDetails(ABS_PRESSURE, 0, 255);
	evDev_->setAbsoluteDetails(ABS_MT_PRESSURE, 0, 255);
	evDev_->setAbsoluteDetails(ABS_MT_TOUCH_MAJOR, 0, 15 * params.width);

	auto enabled = co_await port_->submitCommand(device_cmd::EnableScan{});
	assert(enabled);

	// Create an mbus object for the partition.
	mbus_ng::Properties descriptor{
		{"unix.subsystem", mbus_ng::StringItem{"input"}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(port_->mbusParent())}},
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
		"ps2mouse", descriptor)).unwrap();

	[] (auto evDev, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			libevbackend::serveDevice(evDev, std::move(localLane));
		}
	}(evDev_, std::move(entity));

	processReports();
}

async::detached ElantechTouchpadDevice::processReports() {
	std::println("ps2-hid: running Elantech processing");

	while (true) {
		assert(hwVersion == 4);
		std::array<uint8_t, 6> packet = {
			(co_await port_->pullByte()).value(),
			(co_await port_->pullByte()).value(),
			(co_await port_->pullByte()).value(),
			(co_await port_->pullByte()).value(),
			(co_await port_->pullByte()).value(),
			(co_await port_->pullByte()).value(),
		};

		Packet<4>::parse(*this, std::move(packet));
	}
}

async::result<std::expected<std::array<uint8_t, 3>, Ps2Error>>
ElantechTouchpadDevice::submitQuery(Query query) {
	if (hwVersion >= 3) {
		if (auto r = co_await port_->transferByte(0xF8); !r || *r != 0xFA)
			co_return std::unexpected{Ps2Error::invalid};
		if (auto r = co_await port_->transferByte(std::to_underlying(query)); !r || *r != 0xFA)
			co_return std::unexpected{Ps2Error::invalid};
	} else {
		auto sliced =
		    co_await port_->submitCommand(device_cmd::SlicedCommand{}, std::to_underlying(query));
		if (!sliced)
			co_return std::unexpected{Ps2Error::invalid};
	}

	auto data = co_await mouse_.submitCommand(device_cmd::GetStatus{});
	if (!data)
		co_return std::unexpected{Ps2Error::invalid};

	co_return data.value();
}

async::result<std::optional<Ps2Error>> ElantechTouchpadDevice::writeRegister(uint8_t reg, uint8_t val) {
	if (auto r = co_await port_->transferByte(0xF8); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0x00); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0xF8); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(reg); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0xF8); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0x00); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0xF8); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(val); !r || *r != 0xFA)
		co_return Ps2Error::invalid;
	if (auto r = co_await port_->transferByte(0xE6); !r || *r != 0xFA)
		co_return Ps2Error::invalid;

	co_return std::nullopt;
}

void ElantechTouchpadDevice::Packet<4>::parse(ElantechTouchpadDevice &dev, std::array<uint8_t, 6> packet) {
	auto packet_type = packet[3] & 0x03;

	switch (packet_type) {
		case 0: {
			uint8_t fingerMask = packet[1] & 0x1F;
			for (int i = 0; i < 5; i++) {
				if (!(fingerMask & (1 << i))) {
					dev.evDev_->emitEvent(EV_ABS, ABS_MT_SLOT, i);
					dev.evDev_->emitEvent(EV_ABS, ABS_MT_TRACKING_ID, -1);
					dev.fingers_[i].trackingId = std::nullopt;
					dev.fingers_[i].pressure = 0;
				}
			}
			break;
		}
		case 1: {
			auto id = ((packet[3] & 0xE0) >> 5) - 1;
			if (id < 0 || id >= 5)
				return;

			dev.fingers_[id].x = ((packet[1] & 0x0f) << 8) | packet[2];
			dev.fingers_[id].y = dev.params.y_max - (((packet[4] & 0x0f) << 8) | packet[5]);
			dev.fingers_[id].pressure = (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
			auto traces = (packet[0] & 0xF0) >> 4;
			auto trackingId = [&] {
				if (!dev.fingers_[id].trackingId)
					dev.fingers_[id].trackingId = dev.nextTrackingId_++;
				return *dev.fingers_[id].trackingId;
			}();

			dev.evDev_->emitEvent(EV_ABS, ABS_MT_SLOT, id);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_TRACKING_ID, trackingId);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_X, dev.fingers_[id].x);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_Y, dev.fingers_[id].y);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_PRESSURE, dev.fingers_[id].pressure);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, traces * dev.params.width);
			break;
		}
		case 2: {
			auto id = ((packet[3] & 0xE0) >> 5) - 1;
			if (id < 0 || id >= 5)
				return;

			auto sid = ((packet[3] & 0xe0) >> 5) - 1;
			auto weight = (packet[0] & 0x10) ? 5 : 1;

			int8_t delta_x1 = static_cast<int8_t>(packet[1]);
			int8_t delta_y1 = static_cast<int8_t>(packet[2]);
			int8_t delta_x2 = static_cast<int8_t>(packet[4]);
			int8_t delta_y2 = static_cast<int8_t>(packet[5]);

			dev.fingers_[id].x += delta_x1 * weight;
			dev.fingers_[id].y -= delta_y1 * weight;

			dev.evDev_->emitEvent(EV_ABS, ABS_MT_SLOT, id);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_X, dev.fingers_[id].x);
			dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_Y, dev.fingers_[id].y);

			if (sid >= 0 && sid < 5) {
				dev.fingers_[sid].x += delta_x2 * weight;
				dev.fingers_[sid].y -= delta_y2 * weight;
				dev.evDev_->emitEvent(EV_ABS, ABS_MT_SLOT, sid);
				dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_X, dev.fingers_[sid].x);
				dev.evDev_->emitEvent(EV_ABS, ABS_MT_POSITION_Y, dev.fingers_[sid].y);
			}

			break;
		}
		default:
			std::println("ps2-hid: invalid Elantech packet");
			return;
	}

	if (dev.fwVersion & 0x1000) {
		dev.evDev_->emitEvent(EV_KEY, BTN_LEFT, bool(packet[0] & 0x03));
	} else {
		dev.evDev_->emitEvent(EV_KEY, BTN_LEFT, packet[0] & 1);
		dev.evDev_->emitEvent(EV_KEY, BTN_RIGHT, packet[0] & 2);
		dev.evDev_->emitEvent(EV_KEY, BTN_MIDDLE, packet[0] & 4);
	}

	auto activeFingers =
	    std::ranges::count_if(dev.fingers_, [](auto f) { return f.trackingId.has_value(); });

	dev.evDev_->emitEvent(EV_KEY, BTN_TOUCH, bool(activeFingers));
	dev.evDev_->emitEvent(EV_KEY, BTN_TOOL_FINGER, activeFingers == 1);
	dev.evDev_->emitEvent(EV_KEY, BTN_TOOL_DOUBLETAP, activeFingers == 2);
	dev.evDev_->emitEvent(EV_KEY, BTN_TOOL_TRIPLETAP, activeFingers == 3);
	dev.evDev_->emitEvent(EV_KEY, BTN_TOOL_QUADTAP, activeFingers == 4);
	dev.evDev_->emitEvent(EV_KEY, BTN_TOOL_QUINTTAP, activeFingers == 5);

	auto oldestFinger = [&]() -> std::optional<std::size_t> {
		auto active_fingers =
		    dev.fingers_ | std::views::enumerate | std::views::filter([](const auto &tuple) {
			    return std::get<1>(tuple).trackingId.has_value();
		    });

		if (std::ranges::empty(active_fingers))
			return std::nullopt;

		auto oldest_it = std::ranges::min_element(active_fingers, [](const auto &a, const auto &b) {
			uint16_t id_a = static_cast<uint16_t>(*std::get<1>(a).trackingId);
			uint16_t id_b = static_cast<uint16_t>(*std::get<1>(b).trackingId);
			return static_cast<uint16_t>(id_a - id_b) > 32767;
		});

		return std::get<0>(*oldest_it);
	}();

	if (oldestFinger) {
		auto &f = dev.fingers_[*oldestFinger];

		dev.evDev_->emitEvent(EV_ABS, ABS_X, f.x);
		dev.evDev_->emitEvent(EV_ABS, ABS_Y, f.y);
		dev.evDev_->emitEvent(EV_ABS, ABS_PRESSURE, f.pressure);
	}

	dev.evDev_->emitEvent(EV_SYN, SYN_REPORT, 0);
	dev.evDev_->notify();
}
