#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <helix/ipc.hpp>
#include <libevbackend.hpp>
#include <array>
#include <vector>
#include <memory>
#include <functional>
#include <variant>
#include <type_traits>

struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

struct NoDevice {};

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

struct Controller {
	Controller();
	async::detached init();

	struct Device {
		Device(Controller *controller, int port);

		async::result<void> init();

		bool exists();

		void pushByte(uint8_t byte);

		struct DeviceType {
			bool keyboard;
			bool mouse;

			bool hasScrollWheel;
			bool has5Buttons;
		};

		async::result<std::variant<NoDevice, std::monostate>> submitCommand(device_cmd::DisableScan tag);
		async::result<std::variant<NoDevice, std::monostate>> submitCommand(device_cmd::EnableScan tag);
		async::result<std::variant<NoDevice, DeviceType>> submitCommand(device_cmd::Identify tag);

		async::result<std::variant<NoDevice, std::monostate>> submitCommand(device_cmd::SetReportRate tag, int rate);

		async::result<std::variant<NoDevice, std::monostate>> submitCommand(device_cmd::SetScancodeSet tag, int set);
		async::result<std::variant<NoDevice, int>> submitCommand(device_cmd::GetScancodeSet tag);

	private:
		void sendByte(uint8_t byte);
		async::result<std::optional<uint8_t>> transferByte(uint8_t byte);
		async::result<std::optional<uint8_t>> recvByte(uint64_t timeout = 0);

		async::result<void> mouseInit();
		async::result<void> kbdInit();

		async::detached runMouse();
		async::detached runKbd();

		Controller *_controller;
		int _port;
		DeviceType _deviceType;
		bool _exists;

		async::queue<uint8_t, stl_allocator> _responseQueue;
		async::queue<uint8_t, stl_allocator> _reportQueue;
		bool _awaitingResponse;

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
	std::optional<uint8_t> recvByte(uint64_t timeout = 0);

	std::array<Device *, 2> _devices;
	bool _hasSecondPort;

	arch::io_space _space;

	HelHandle _irq1Handle;
	HelHandle _irq12Handle;
	helix::UniqueIrq _irq1;
	helix::UniqueIrq _irq12;

};

