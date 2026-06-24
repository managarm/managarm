#pragma once

#include <async/result.hpp>

#include "../ps2.hpp"

struct ElantechTouchpadDevice final : Controller::Device {
	enum class Query : uint8_t {
		FwId = 0x00,
		FwVersion = 0x01,
		Capabilities = 0x02,
		Sample = 0x03,
		Resolution = 0x04,
		IcBody = 0x05,
	};

	struct ProbeData {
		uint32_t fw_version;
		uint8_t hw_version;
	};

	ElantechTouchpadDevice(Controller::Port *port, ProbeData data)
	: port_{port},
	  mouse_{port},
	  fwVersion{data.fw_version},
	  hwVersion{data.hw_version} {}

	static async::result<std::expected<ProbeData, Ps2Error>> probe(Controller::Port *port);

	async::result<void> run() override;

private:
	template <size_t Version>
	struct Packet;

	template<>
	struct Packet<4> {
		static void parse(ElantechTouchpadDevice &dev, std::array<uint8_t, 6> packet);
	};

	async::detached processReports();

	async::result<std::expected<std::array<uint8_t, 3>, Ps2Error>> submitQuery(Query);
	async::result<std::optional<Ps2Error>> writeRegister(uint8_t reg, uint8_t val);

	Controller::Port *port_;
	Controller::MouseDevice mouse_;
	std::shared_ptr<libevbackend::EventDevice> evDev_;

	uint32_t fwVersion;
	uint8_t hwVersion;

	uint8_t icVersion() const {
		return (fwVersion >> 16) & 0x0F;
	}

	struct FingerPosition {
		std::optional<int> trackingId = std::nullopt;
		unsigned x;
		unsigned y;
		int pressure = 0;
	};

	std::array<FingerPosition, 5> fingers_;
	uint16_t nextTrackingId_ = 0;

	struct Params {
		unsigned x_max;
		unsigned y_max;
		unsigned traces;
		unsigned width;
	} params;
};
