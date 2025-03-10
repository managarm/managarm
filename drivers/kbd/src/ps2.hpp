#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <helix/ipc.hpp>
#include <libevbackend.hpp>
#include <array>
#include <memory>
#include <protocols/hw/client.hpp>

struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

struct NoDevice {};

enum class Ps2Error {
	none,
	timeout,
	nack
};

namespace controller_cmd {
	struct DisablePort {};
	struct EnablePort {};
	struct GetByte0 {};
	struct SetByte0 {};
	struct SendBytePort2 {};
}

namespace device_cmd {
	struct DisableScan {};
	struct EnableScan {};
	struct Identify {};

	// mouse specific
	struct SetReportRate {};

	// keyboard specific
	struct SetScancodeSet {};
	struct GetScancodeSet {};
}

template<typename T>
struct FlagGuard {
	FlagGuard(T &flag, T target)
	: _flag{flag}, _target{target} {}
	~FlagGuard() {
		_flag = _target;
	}

private:
	T &_flag;
	T _target;
};

struct DeviceType {
	bool keyboard = false;
	bool mouse = false;

	bool hasScrollWheel = false;
	bool has5Buttons = false;
};

struct Controller {
	Controller(std::shared_ptr<protocols::hw::Device> device, std::array<uintptr_t, 2> ports,
		std::optional<std::pair<int, std::shared_ptr<protocols::hw::Device>>> primaryIrq,
		std::optional<std::pair<int, std::shared_ptr<protocols::hw::Device>>> secondaryIrq);
	async::detached init();

	struct Device {
		virtual ~Device() = default;

	public:
		virtual async::result<void> run() = 0;
	};

	struct Port {
		Port(Controller *controller, int port);

		async::result<void> init();

		int getIndex() {
			return _port;
		}

		bool isDead() {
			return _dead;
		}

		DeviceType deviceType() {
			return _deviceType;
		}

		void pushByte(uint8_t byte);
		async::result<std::optional<uint8_t>> pullByte(async::cancellation_token ct = {});

		async::result<frg::expected<Ps2Error>>
		submitCommand(device_cmd::DisableScan tag);

		async::result<frg::expected<Ps2Error>>
		submitCommand(device_cmd::EnableScan tag);

		async::result<frg::expected<Ps2Error, DeviceType>>
		submitCommand(device_cmd::Identify tag);

		void sendByte(uint8_t byte);
		async::result<std::optional<uint8_t>> transferByte(uint8_t byte);
		async::result<std::optional<uint8_t>> recvResponseByte(uint64_t timeout = 0);

	private:
		Controller *_controller;
		int _port;
		DeviceType _deviceType;
		bool _dead = false;

		async::queue<uint8_t, stl_allocator> _dataQueue;
		std::unique_ptr<Device> _device;
	};

	struct KbdDevice final : Device {
		KbdDevice(Port *port)
		: _port{port} { }

		virtual async::result<void> run() override;

	private:
		async::result<frg::expected<Ps2Error>>
		submitCommand(device_cmd::SetScancodeSet tag, int set);

		async::result<frg::expected<Ps2Error, int>>
		submitCommand(device_cmd::GetScancodeSet tag);

		async::detached processReports();

		Port *_port;
		std::shared_ptr<libevbackend::EventDevice> _evDev;
		int _codeSet = 0;
	};

	struct MouseDevice final : Device {
		MouseDevice(Port *port)
		: _port{port} { }

		virtual async::result<void> run() override;

	private:
		async::result<frg::expected<Ps2Error>>
		submitCommand(device_cmd::SetReportRate tag, int rate);

		async::detached processReports();

		Port *_port;
		DeviceType _deviceType;
		std::shared_ptr<libevbackend::EventDevice> _evDev;
	};

	async::detached handleIrqsFor(helix::UniqueIrq &irq, int port);

	void submitCommand(controller_cmd::DisablePort tag, int port);
	void submitCommand(controller_cmd::EnablePort tag, int port);
	uint8_t submitCommand(controller_cmd::GetByte0 tag);
	void submitCommand(controller_cmd::SetByte0 tag, uint8_t val);
	void submitCommand(controller_cmd::SendBytePort2 tag);

private:
	bool processData(int port);

	void sendCommandByte(uint8_t byte);
	void sendDataByte(uint8_t byte);
	std::optional<uint8_t> recvResponseByte(uint64_t timeout = 0);

	std::array<Port *, 2> _ports{};
	bool _hasSecondPort = true;
	bool _portsOwnData = false;

	std::shared_ptr<protocols::hw::Device> _hwDevice;
	std::array<uintptr_t, 2> _ioPorts;

	arch::io_space _dataSpace;
	arch::io_space _commandSpace;

	std::optional<std::pair<int, std::shared_ptr<protocols::hw::Device>>> _primaryIrq;
	std::optional<std::pair<int, std::shared_ptr<protocols::hw::Device>>> _secondaryIrq;

	helix::UniqueIrq _port1Irq;
	std::optional<helix::UniqueIrq> _port2Irq;

};

