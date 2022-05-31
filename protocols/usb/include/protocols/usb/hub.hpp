#pragma once

#include <memory>

#include <arch/dma_structs.hpp>
#include <async/result.hpp>
#include <async/mutex.hpp>
#include <frg/expected.hpp>

#include "usb.hpp"
#include "api.hpp"

// ----------------------------------------------------------------
// Hub.
// ----------------------------------------------------------------

namespace HubStatus {
	static constexpr uint32_t connect = 0x01;
	static constexpr uint32_t enable = 0x02;
	static constexpr uint32_t reset = 0x04;
}

struct PortState {
	uint32_t status;
	uint32_t changes;
};

struct Hub {
protected:
	~Hub() = default;

public:
	Hub(std::shared_ptr<Hub> parent)
	: parent_{parent} { }

	virtual size_t numPorts() = 0;
	virtual async::result<PortState> pollState(int port) = 0;
	virtual async::result<frg::expected<UsbError, DeviceSpeed>> issueReset(int port) = 0;

	std::shared_ptr<Hub> parent() const {
		return parent_;
	}

private:
	std::shared_ptr<Hub> parent_;
};

async::result<frg::expected<UsbError, std::shared_ptr<Hub>>>
createHubFromDevice(std::shared_ptr<Hub> parentHub, Device device);

// ----------------------------------------------------------------
// Enumerator.
// ----------------------------------------------------------------

struct Enumerator {
	Enumerator(BaseController *controller)
	: controller_{controller} { }

	void observeHub(std::shared_ptr<Hub> hub);

private:
	async::detached observePort_(std::shared_ptr<Hub> hub, int port);
	async::result<void> observationCycle_(std::shared_ptr<Hub> hub, int port);

	BaseController *controller_;
	async::mutex enumerateMutex_;
};
