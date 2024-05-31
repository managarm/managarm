#include <helix/timer.hpp>
#include <netserver/nic.hpp>
#include <arch/dma_pool.hpp>
#include <nic/usb_ecm/usb_ecm.hpp>

struct UsbEcmNic : nic::Link {
	UsbEcmNic(protocols::usb::Device hw_device, nic::MacAddress mac, protocols::usb::Endpoint in, protocols::usb::Endpoint out);

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	~UsbEcmNic() override = default;
private:
	arch::contiguous_pool dmaPool_;
	protocols::usb::Device device_;

	protocols::usb::Endpoint in_;
	protocols::usb::Endpoint out_;
};

UsbEcmNic::UsbEcmNic(protocols::usb::Device hw_device, nic::MacAddress mac, protocols::usb::Endpoint in, protocols::usb::Endpoint out)
	: nic::Link(1500, &dmaPool_), device_{std::move(hw_device)}, in_{std::move(in)}, out_{std::move(out)} {
	mac_ = mac;
}

async::result<size_t> UsbEcmNic::receive(arch::dma_buffer_view frame) {
	while(true) {
		auto res = co_await in_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToHost, frame});
		assert(res);

		if(res.value() != 0)
			co_return res.value();
	}
}

async::result<void> UsbEcmNic::send(const arch::dma_buffer_view payload) {
	co_await out_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToDevice, payload});
	co_return;
}

namespace nic::usb_ecm {

std::shared_ptr<nic::Link> makeShared(protocols::usb::Device hw_device, MacAddress mac, protocols::usb::Endpoint in, protocols::usb::Endpoint out) {
	return std::make_shared<UsbEcmNic>(std::move(hw_device), mac, std::move(in), std::move(out));
}

} // namespace nic::usb_ecm
