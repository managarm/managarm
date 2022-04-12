#include <nic/rtl8139/rtl8139.hpp>
#include <arch/io_space.hpp>
#include <arch/dma_pool.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

namespace {

uint16_t readPointer;
/* pci port address */
int port;

struct Rtl8139Nic : nic::Link {

	virtual async::result<void> receive(arch::dma_buffer_view) override;
	virtual async::result<void> send(const arch::dma_buffer_view) override;

	virtual ~Rtl8139Nic() override = default;
private:
	bool isInitialized;
	std::byte * receiveBuffer[8208];

	virtual async::result<void> initialize(mbus::Entity entity);
	virtual async::detached bindDevice();
};

async::result<void> Rtl8139Nic::initialize(mbus::Entity entity) {
	protocols::hw::Device pci_device(co_await entity.bind());
	auto info = co_await pci_device.getPciInfo();
	auto ioBarInfo = info.barInfo[0];
	port = ioBarInfo.address;
	/* get bar */
	auto ioBar = co_await pci_device.accessBar(0);
	/* add io access */
	HEL_CHECK(helEnableIo(ioBar.getHandle()));

	/* rtl8139 specific things */

	/* turn on rtl8139 */
	arch::_detail::io_ops<uint8_t>::store(port + 0x52, 0x0);

	/* Reset the device */
	arch::_detail::io_ops<uint8_t>::store(port + 0x37, 0x10);
	while ((arch::_detail::io_ops<uint8_t>::load(port + 0x37) & 0x10) != 0) {}

	/* init receive buffer */
	memset(receiveBuffer, 0x0, 8208);
	arch::_detail::io_ops<uint32_t>::store(port + 0x30, (uintptr_t)receiveBuffer);

	/* Set TOK and ROK buts high */
	arch::_detail::io_ops<uint16_t>::store(port + 0x3c, 0x0005);

	/* Accept all possible packets */
	arch::_detail::io_ops<uint32_t>::store(port + 0x44, 0xf | (1 << 7));

	/* Enable Receive and Transmitter */
	arch::_detail::io_ops<uint8_t>::store(port + 0x37, 0x0c);
}

async::detached Rtl8139Nic::bindDevice() {
	/* Mostly generic pci bindings */

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "02"),  /* pci-class for networking */
 		mbus::EqualsFilter("pci-vendor", "15ad"), /* vendorID for rtl8139 */
		mbus::EqualsFilter("pci-device", "0405") /* deviceID for rtl8139 */
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([this] (mbus::Entity entity, mbus::Properties) {
		printf("drivers: nic: rtl8139: detected controller\n");
		initialize(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}


async::result<void> Rtl8139Nic::receive(arch::dma_buffer_view frame) {
	/* check if the driver was initialized, if not, init */
	if (!isInitialized)
		bindDevice();

	/* rtl 8139 specific stuff */

	std::byte * receiveOut;
	memcpy(receiveOut, receiveBuffer, 8208);

	/*
	 * Saw this algorithm pretty much everywhere, supposedly it updates
	 * the read pointer, no example said why or how though...
	 * im not going to hunt through the datasheets for an answer as
	 * pretty much every example driver has this.
	*/
	readPointer = (readPointer + sizeof(receiveOut) + 4 + 3) & (~0x3);

	/* Place the read pointer in the card */
	arch::_detail::io_ops<uint16_t>::store(0x38, port - 0x10);

	/* convert receiveOut into the frame */
	frame.size =  8208;

	frame.byte_data = receiveOut;

	memcpy(frame.data(), receiveOut, 8208);
}


async::result<void> Rtl8139Nic::send(const arch::dma_buffer_view payload) {
	if (payload.size() > 8208) {
		throw std::runtime_error("data exceeds capacity");
	}



}
} // namespace
