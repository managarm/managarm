#include <async/basic.hpp>
#include <memory>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/regs.hpp>
#include <unistd.h>

Intel8254xNic::Intel8254xNic(protocols::hw::Device device)
	: nic::Link(1500, &_dmaPool), _device{std::move(device)} {
		async::run(this->init(), helix::currentDispatcher);
}

async::result<void> Intel8254xNic::init() {
	auto info = co_await _device.getPciInfo();
	_irq = co_await _device.accessIrq();
	co_await _device.enableBusmaster();

	auto &barInfo = info.barInfo[0];
	assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar0 = co_await _device.accessBar(0);

	_mmio_mapping = {bar0, barInfo.offset, barInfo.length};
	_mmio = _mmio_mapping.get();

	_mmio.store(regs::ctrl, flags::ctrl::reset(true));
	/* p. 228, Table 13-3: To ensure that global device reset has fully completed and that the Ethernet controller
	 * responds to subsequent access, wait approximately 1 µs after setting and before attempting to check to see
	 * if the bit has cleared or to access any other device register. */
	usleep(1);
	while(_mmio.load(regs::ctrl) & flags::ctrl::reset)
		usleep(1);

	_mmio.store(regs::ctrl, _mmio.load(regs::ctrl) / flags::ctrl::asde(true) / flags::ctrl::set_link_up(true) / flags::ctrl::lrst(false) / flags::ctrl::phy_reset(false) / flags::ctrl::ilos(false));

	_mmio.store(regs::fcal, 0);
	_mmio.store(regs::fcah, 0);
	_mmio.store(regs::fct, 0);
	_mmio.store(regs::fcttv, 0);

	_mmio.store(regs::ctrl, _mmio.load(regs::ctrl) / flags::ctrl::vme(false));

	for(size_t i = 0; i < 128; i++) {
		_mmio.store(arch::scalar_register<uint32_t>(regs::mta.offset() + (i * 4)), 0);
	}

	auto eeprom_present = _mmio.load(regs::eecd) & flags::eecd::present;

	if(!eeprom_present) {
		std::cout << "i8254x: EEPROM not present, aborting";
		co_return;
	}

	uint16_t m = co_await eepromRead(0);
	mac_[0] = (m & 0xFF);
	mac_[1] = ((m >> 8) & 0xFF);
	m = co_await eepromRead(1);
	mac_[2] = (m & 0xFF);
	mac_[3] = ((m >> 8) & 0xFF);
	m = co_await eepromRead(2);
	mac_[4] = (m & 0xFF);
	mac_[5] = ((m >> 8) & 0xFF);

	_mmio.store(regs::ral_0, *(uint32_t *) mac_.data());
	_mmio.store(regs::rah_0, *(uint16_t *) (mac_.data() + 4));

	if constexpr (logDebug) std::cout << "i8254x: MAC " << mac_ << std::endl;

	enableIrqs();
}

void Intel8254xNic::enableIrqs() {
	_mmio.store(regs::ims, 0xFF & ~4);
	_mmio.load(regs::icr);
}

async::result<uint16_t> Intel8254xNic::eepromRead(uint8_t address) {
	_mmio.store(regs::eerd, flags::eerd::start(true) / flags::eerd::addr(address));

	auto res = _mmio.load(regs::eerd);
	while(!(res & flags::eerd::done)) {
		usleep(1);
		res = _mmio.load(regs::eerd);
	}

	co_return res & flags::eerd::data;
}

namespace nic::intel8254x {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device) {
	return std::make_shared<Intel8254xNic>(std::move(device));
}

} // namespace nic::intel8254x
