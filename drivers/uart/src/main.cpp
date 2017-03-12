
#include <iostream>

#include <stdio.h>
#include <string.h>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "fs.pb.h"

arch::io_space base;
helix::UniqueIrq irq;

void send(char c) {
	while(!(base.load(uart_register::lineStatus) & line_status::txReady)) {
		// do nothing until the UART is ready to transmit.
	}
	base.store(uart_register::data, c);
}

void sendString(const char *str) {
	while(*str != 0)
		send(*str++);
}

COFIBER_ROUTINE(cofiber::no_future, handleIrqs(), ([=] {
	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(irq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());
		
		while(base.load(uart_register::lineStatus) & line_status::dataReady) {
			auto c = base.load(uart_register::data);
			printf("%c\n", c);
		}
	}
}))
	
async::result<void> seek(std::shared_ptr<void> object, uintptr_t offset) {
	throw std::runtime_error("seek not yet implemented");
}

async::result<void> read(std::shared_ptr<void> object, void *buffer, size_t length) {
	throw std::runtime_error("read not yet implemented");
}

COFIBER_ROUTINE(async::result<void>, write(std::shared_ptr<void> object,
		const void *buffer, size_t length), ([=] {
	sendString(reinterpret_cast<const char *>(buffer));	
	COFIBER_RETURN();
}))

async::result<helix::BorrowedDescriptor> accessMemory(std::shared_ptr<void> object) {
	throw std::runtime_error("accessMemory not yet implemented");
}

constexpr protocols::fs::FileOperations fileOperations {
	&seek,
	&read,
	&write,
	&accessMemory
};

COFIBER_ROUTINE(cofiber::no_future, serveTerminal(helix::UniqueLane p),
		([lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::servePassthrough(std::move(local_lane), nullptr,
					&fileOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid serveTerminal request!");
		}
	}
}))
	

COFIBER_ROUTINE(cofiber::no_future, runTerminal(), ([=] {
	// Create an mbus object for the partition.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	std::unordered_map<std::string, std::string> descriptor {
		{ "unix.devtype", "block" },
		{ "unix.devname", "ttyS0" },
	};
	auto object = COFIBER_AWAIT root.createObject("uart0", descriptor,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveTerminal(std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
}))

int main() {
	printf("Starting UART driver\n");

	HelHandle irq_handle;
	HEL_CHECK(helAccessIrq(4, &irq_handle));
	irq = helix::UniqueIrq(irq_handle);

	uintptr_t ports[] = { COM1, COM1 + 1, COM1 + 2, COM1 + 3, COM1 + 4, COM1 + 5, COM1 + 6,
			COM1 + 7 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 8, &handle));
	HEL_CHECK(helEnableIo(handle));
	
	base = arch::global_io.subspace(COM1);
	
	// Set the baud rate.
	base.store(uart_register::lineControl, line_control::dlab(true));
	base.store(uart_register::baudLow, BaudRate::low9600);
	base.store(uart_register::baudHigh, BaudRate::high9600);

	base.store(uart_register::lineControl, line_control::dataBits(DataBits::charLen8) 
			| line_control::stopBit(StopBits::one) | line_control::parityBits(Parity::none)
			| line_control::dlab(false));
	
	base.store(uart_register::fifoControl, fifo_control::fifoEnable(FifoCtrl::enable)
			| fifo_control::fifoIrqLvl(FifoCtrl::triggerLvl14));
	
	base.store(uart_register::irq, irq_control::irqEnable(IrqCtrl::enable));

	runTerminal();

	handleIrqs();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

