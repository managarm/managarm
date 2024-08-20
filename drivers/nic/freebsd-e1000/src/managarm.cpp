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

void E1000Nic::pciRead(u32 reg, u32 *value) {
	auto run = [this, reg]() -> async::result<u16> {
		u32 ret = co_await _device.loadPciSpace(reg, 4);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

void E1000Nic::pciRead(u32 reg, u16 *value) {
	auto run = [this, reg]() -> async::result<u16> {
		u16 ret = co_await _device.loadPciSpace(reg, 2);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

void E1000Nic::pciRead(u32 reg, u8 *value) {
	auto run = [this, reg]() -> async::result<u16> {
		u8 ret = co_await _device.loadPciSpace(reg, 1);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device) {
	return std::make_shared<E1000Nic>(std::move(device));
}

} // namespace nic::intel8254x
