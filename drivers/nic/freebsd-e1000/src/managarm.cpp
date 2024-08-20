#include <async/basic.hpp>
#include <helix/ipc.hpp>
#include <nic/freebsd-e1000/common.hpp>

E1000Nic::E1000Nic(protocols::hw::Device device)
	: nic::Link(1500, &_dmaPool), _device{std::move(device)},
	_rxIndex(0, RX_QUEUE_SIZE), _txIndex(0, TX_QUEUE_SIZE) {
	async::run(this->init(), helix::currentDispatcher);
}

async::result<void> E1000Nic::init() {
	auto info = co_await _device.getPciInfo();
	_irq = co_await _device.accessIrq();
	co_await _device.enableBusmaster();

	co_return;
}

async::result<size_t> E1000Nic::receive(arch::dma_buffer_view frame) {
	co_return {};
}

async::result<void> E1000Nic::send(const arch::dma_buffer_view buf) {
	co_return;
}

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device) {
	return std::make_shared<E1000Nic>(std::move(device));
}

} // namespace nic::intel8254x
