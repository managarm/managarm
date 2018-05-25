
#include <algorithm>
#include <deque>
#include <iostream>

#include <stdio.h>
#include <string.h>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "fs.pb.h"

arch::io_space base;
helix::UniqueIrq irq;

struct ReadRequest {
	ReadRequest(void *buffer, size_t maxLength)
	: buffer(buffer), maxLength(maxLength) { }

	void *buffer;
	size_t maxLength;
	async::promise<size_t> promise;
	boost::intrusive::list_member_hook<> hook;
};
boost::intrusive::list<
	ReadRequest,
	boost::intrusive::member_hook<
		ReadRequest,
		boost::intrusive::list_member_hook<>,
		&ReadRequest::hook
	>
> recvRequests;
std::deque<uint8_t> recvBuffer;

void processRecv() {
	while(!recvRequests.empty() && !recvBuffer.empty()) {
		auto req = &recvRequests.front();
	
		size_t read_size = std::min(req->maxLength, recvBuffer.size());
		for(size_t i = 0; i < read_size; i++) {
			reinterpret_cast<uint8_t*>(req->buffer)[i] = recvBuffer[0];
			recvBuffer.pop_front();
		}

		req->promise.set_value(read_size);
		recvRequests.pop_front();
		delete req;
	}
}

struct WriteRequest {
	WriteRequest(const void *buffer, size_t length)
	: buffer(buffer), length(length), progress(0) { }

	const void *buffer;
	size_t length;
	size_t progress;
	async::promise<void> promise;
	boost::intrusive::list_member_hook<> hook;
};
boost::intrusive::list<
	WriteRequest,
	boost::intrusive::member_hook<
		WriteRequest,
		boost::intrusive::list_member_hook<>,
		&WriteRequest::hook
	>
> sendRequests;

void sendBurst() {
	if(sendRequests.empty())
		return;
	
	auto req = &sendRequests.front();
	size_t send_size = std::min(req->length - req->progress, (size_t)16);
	for(size_t i = 0; i < send_size; i++) {
		base.store(uart_register::data, reinterpret_cast<const char *>(req->buffer)
				[req->progress + i]);
	}
	req->progress += send_size;
	
	if(req->progress >= req->length) {
		req->promise.set_value();
		sendRequests.pop_front();
		delete req;
	}
}

COFIBER_ROUTINE(cofiber::no_future, handleIrqs(), ([=] {
	uint64_t sequence = 0;
	while(true) {
		std::cout << "uart: Awaiting IRQ." << std::endl;
		helix::AwaitEvent await_irq;
		auto &&submit = helix::submitAwaitEvent(irq, &await_irq, sequence,
				helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());
		sequence = await_irq.sequence();
		std::cout << "uart: IRQ fired." << std::endl;
		
		auto reason = base.load(uart_register::irqIdentification);
		if(reason & irq_ident_register::ignore)
			continue;
		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), 0, sequence));

		if((reason & irq_ident_register::id) == IrqIds::lineStatus) {
			printf("Overrun, Parity, Framing or Break Error!\n");
		}else if((reason & irq_ident_register::id) == IrqIds::dataAvailable
				|| (reason & irq_ident_register::id) == IrqIds::charTimeout) {
			while(base.load(uart_register::lineStatus) & line_status::dataReady) {
				auto c = base.load(uart_register::data);
				recvBuffer.push_back(c);
			}
			processRecv();
		}else if((reason & irq_ident_register::id) == IrqIds::txEmpty) {
			sendBurst();
		}else if((reason & irq_ident_register::id) == IrqIds::modem) {
			printf("Modem detected!\n");
		}
	}
}))
	
COFIBER_ROUTINE(async::result<protocols::fs::ReadResult>,
read(void *, const char *, void *buffer, size_t length), ([=] {
	auto req = new ReadRequest(buffer, length);
	recvRequests.push_back(*req);
	auto future = req->promise.async_get();
	processRecv();
	auto value = COFIBER_AWAIT std::move(future);
	COFIBER_RETURN(value);
}))

async::result<void> write(void *, const char *, const void *buffer, size_t length) {
	auto req = new WriteRequest(buffer, length);
	sendRequests.push_back(*req);
	auto value = req->promise.async_get();
	if(base.load(uart_register::lineStatus) & line_status::txReady)
		sendBurst();
	return value;
}

async::result<protocols::fs::AccessMemoryResult> accessMemory(void *,
		uint64_t, size_t) {
	throw std::runtime_error("accessMemory not yet implemented");
}

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&read)
	.withWrite(&write)
	.withAccessMemory(&accessMemory);

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
	
	mbus::Properties descriptor{
		{"unix.devtype", mbus::StringItem{"block"}},
		{"unix.devname", mbus::StringItem{"ttyS0"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveTerminal(std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject("uart0", descriptor, std::move(handler));
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
	
	base.store(uart_register::irqEnable, irq_enable::dataAvailable(IrqCtrl::enable)
			| irq_enable::txEmpty(IrqCtrl::enable)
			| irq_enable::lineStatus(IrqCtrl::enable));

	runTerminal();

	handleIrqs();

	helix::globalQueue()->run();
	
	return 0;
}

