#include <format>
#include <nic/usb_net/usb_net.hpp>

#include "usb-ecm.hpp"
#include "usb-net.hpp"

namespace nic::usb_ecm {

UsbEcmNic::UsbEcmNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface data_intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out)
	: UsbNic{hw_device, mac, ctrl_intf, ctrl_ep, data_intf, in, out}, entity_{entity} {

}

async::detached UsbEcmNic::listenForNotifications() {
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
				l1_up_ = (notification->wValue == 1);
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

async::result<void> UsbEcmNic::initialize() {
	arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&dmaPool_};
	ctrl_msg->type = protocols::usb::setup_type::byClass | protocols::usb::setup_type::targetInterface;
	ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::SET_ETHERNET_PACKET_FILTER);
	ctrl_msg->value = 0xF;
	ctrl_msg->index = ctrl_intf_.num();
	ctrl_msg->length = 0;

	auto res = co_await device_.transfer(protocols::usb::ControlTransfer{
		protocols::usb::kXferToDevice, ctrl_msg, {}
	});
	assert(res);

	auto config_val = (co_await device_.currentConfigurationValue()).value();

	mbus_ng::Properties descriptor = {
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"usb.interface_classes", mbus_ng::ArrayItem{{
			mbus_ng::ArrayItem{{
				mbus_ng::StringItem{std::format("{}.{}", config_val, ctrl_intf_.num())},
				mbus_ng::StringItem{"net"},
			}},
		}}},
	};
	descriptor.merge(mbusNetworkProperties());

	auto device_entity = (co_await mbus_ng::Instance::global().createEntity("usb-ncm", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));
		}
	}(std::move(device_entity));

	listenForNotifications();
}

async::result<size_t> UsbEcmNic::receive(arch::dma_buffer_view frame) {
	while(true) {
		auto res = co_await data_in_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToHost, frame});
		assert(res);

		if(res.value() != 0)
			co_return res.value();
	}
}

async::result<void> UsbEcmNic::send(const arch::dma_buffer_view payload) {
	auto res = co_await data_out_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToDevice, payload});
	assert(res);
	co_return;
}

} // namespace nic::usb_ecm
