#include <nic/usb_net/usb_net.hpp>

#include "usb-ncm.hpp"
#include "usb-net.hpp"

namespace nic::usb_ncm {

UsbNcmNic::UsbNcmNic(protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface data_intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out)
	: UsbNic{hw_device, mac, ctrl_intf, ctrl_ep, data_intf, in, out} {

}

async::result<void> UsbNcmNic::initialize() {
	arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&dmaPool_};
	arch::dma_object<NtbParameter> params{&dmaPool_};
	ctrl_msg->type = protocols::usb::setup_type::byClass |
					protocols::usb::setup_type::toHost | protocols::usb::setup_type::targetInterface;
	ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::GET_NTB_PARAMETERS);
	ctrl_msg->value = 0;
	ctrl_msg->index = ctrl_intf_.num();
	ctrl_msg->length = params.view_buffer().size();

	auto res = co_await device_.transfer(protocols::usb::ControlTransfer{
		protocols::usb::kXferToHost, ctrl_msg, params.view_buffer()
	});
	assert(res);
	std::cout << std::format("{}", *params) << std::endl;

	// ctrl_msg->type = protocols::usb::setup_type::byClass | protocols::usb::setup_type::targetInterface;
	// ctrl_msg->request = 0x8A;
	// ctrl_msg->value = 0;
	// ctrl_msg->index = info_.ctrl_intf.num();
	// ctrl_msg->length = 0;

	// res = co_await device_.transfer(protocols::usb::ControlTransfer{
	// 	protocols::usb::kXferToDevice, ctrl_msg, {}
	// });
	// assert(res);

	ctrl_msg->type = protocols::usb::setup_type::byClass | protocols::usb::setup_type::targetInterface;
	ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::SET_ETHERNET_PACKET_FILTER);
	ctrl_msg->value = 0xF;
	ctrl_msg->index = ctrl_intf_.num();
	ctrl_msg->length = 0;

	res = co_await device_.transfer(protocols::usb::ControlTransfer{
		protocols::usb::kXferToDevice, ctrl_msg, {}
	});
	assert(res);

	listenForNotifications();
}

async::detached UsbNcmNic::listenForNotifications() {
	using NotificationHeader = protocols::usb::CdcNotificationHeader;

	while(true) {
		arch::dma_buffer report{device_.bufferPool(), 16};
		protocols::usb::InterruptTransfer transfer{protocols::usb::XferFlags::kXferToHost, report};
		transfer.allowShortPackets = true;
		auto length = (co_await ctrl_ep_.transfer(transfer)).unwrap();

		assert(length >= sizeof(NotificationHeader));

		auto notification = reinterpret_cast<NotificationHeader *>(report.data());

		switch(notification->bNotificationCode) {
			using Notification = NotificationHeader::Notification;

			case Notification::NETWORK_CONNECTION:
				printf("netserver: connection %s\n", notification->wValue == 1 ? "up" : "down");
				break;
			case Notification::CONNECTION_SPEED_CHANGE: {
				auto change = reinterpret_cast<protocols::usb::CdcConnectionSpeedChange *>(report.subview(sizeof(protocols::usb::CdcNotificationHeader)).data());
				printf("netserver: connection speed %u MBit/s\n", change->DlBitRate / 1000 / 1000);
				break;
			}
			default: {
				printf("netserver: received notification 0x%x\n", uint8_t(notification->bNotificationCode));
				break;
			}
		}
	}

	co_return;
}

async::result<size_t> UsbNcmNic::receive(arch::dma_buffer_view frame) {
	arch::dma_buffer buf{&dmaPool_, 0x1000};

	auto res = co_await data_in_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToHost, buf});
	assert(res);

	if(res.value() != 0) {
		auto ncmHeader = reinterpret_cast<NcmTransferHeader *>(buf.data());
		auto ndp = reinterpret_cast<NcmDatagramPointer *>(buf.subview(ncmHeader->wNdpIndex).data());
		memcpy(frame.data(), buf.subview(ndp->wDatagram[0].Index).data(), ndp->wDatagram[0].Length);
		co_return ndp->wDatagram[0].Length;
	}

	assert(!"USB NIC receive failed!");
}

async::result<void> UsbNcmNic::send(const arch::dma_buffer_view payload) {
	arch::dma_buffer buf{&dmaPool_, sizeof(NcmTransferHeader) + sizeof(NcmDatagramPointer) + payload.size()};
	auto ncmHeader = reinterpret_cast<NcmTransferHeader *>(buf.data());
	ncmHeader->dwSignature = NCM_NTH16_SIGNATURE;
	ncmHeader->wHeaderLength = sizeof(*ncmHeader);
	ncmHeader->wSequence = seq_++;
	ncmHeader->wBlockLength = uint16_t(buf.size());
	ncmHeader->wNdpIndex = sizeof(*ncmHeader);

	auto ndp = reinterpret_cast<NcmDatagramPointer *>(buf.subview(ncmHeader->wNdpIndex).data());
	ndp->dwSignature = NCM_NDP16_NO_CRC_SIGNATURE;
	ndp->wLength = sizeof(*ndp);
	ndp->wNextNdpIndex = 0;
	ndp->wDatagram[0].Index = ncmHeader->wHeaderLength + ndp->wLength;
	ndp->wDatagram[0].Length = uint16_t(payload.size());
	ndp->wDatagram[1].Index = 0;
	ndp->wDatagram[1].Length = 0;

	memcpy(buf.subview(ndp->wDatagram[0].Index).data(), payload.data(), payload.size());

	auto res = co_await data_out_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToDevice, buf});
	assert(res);
	co_return;
}

} // namespace nic::usb_ncm
