#include <async/basic.hpp>
#include <protocols/hw/client.hpp>

#include <nic/freebsd-e1000/common.hpp>

E1000Nic::E1000Nic(protocols::hw::Device device)
	: nic::Link(1500, &_dmaPool), _device{std::move(device)}, _rxIndex(0, 32), _txIndex(0, 32) {
	async::run(this->init(), helix::currentDispatcher);
}

async::result<void> E1000Nic::init() {
	auto info = co_await _device.getPciInfo();
	_irq = co_await _device.accessIrq();
	co_await _device.enableBusmaster();
	co_return;
}

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device) {
	return std::make_shared<E1000Nic>(std::move(device));
}

} // namespace nic::intel8254x
