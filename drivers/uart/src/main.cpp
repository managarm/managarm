
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
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "fs.bragi.hpp"

static constexpr bool logIrqs = false;
static constexpr bool logTx = false;

arch::io_space base;
helix::UniqueIrq irq;

struct ReadRequest {
	ReadRequest(void *buffer, size_t maxLength)
	: buffer(buffer), maxLength(maxLength) { }

	void *buffer;
	size_t maxLength;
	size_t progress = 0;
	async::promise<void> promise;
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

void completeRecvs() {
	assert(!recvRequests.empty());
	assert(!recvBuffer.empty());

	boost::intrusive::list<
		ReadRequest,
		boost::intrusive::member_hook<
			ReadRequest,
			boost::intrusive::list_member_hook<>,
			&ReadRequest::hook
		>
	> pending;

	while(!recvRequests.empty() && !recvBuffer.empty()) {
		auto req = &recvRequests.front();
	
		size_t chunk = std::min(req->maxLength, recvBuffer.size());
		assert(chunk);
		for(size_t i = 0; i < chunk; i++) {
			auto p = reinterpret_cast<std::byte *>(req->buffer);
			p[i] = static_cast<std::byte>(recvBuffer.front());
			recvBuffer.pop_front();
		}

		req->progress = chunk;

		// We always complete the request here,
		// even if we did not read req->maxLength bytes yet.
		recvRequests.pop_front();
		pending.push_back(*req);
	}

	while(!pending.empty()) {
		auto req = &pending.front();
		pending.pop_front();
		req->promise.set_value();
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

// Size of the device's TX FIFO in bytes.
constexpr size_t txFifoSize = 16;

bool txInFlight = false;

void flushSends() {
	assert(!sendRequests.empty());
	assert(!txInFlight);

	if(logTx)
		std::cout << "uart: Flushing TX" << std::endl;

	boost::intrusive::list<
		WriteRequest,
		boost::intrusive::member_hook<
			WriteRequest,
			boost::intrusive::list_member_hook<>,
			&WriteRequest::hook
		>
	> pending;

	size_t fifoAvailable = txFifoSize;
	while(!sendRequests.empty() && fifoAvailable) {
		auto req = &sendRequests.front();
		assert(req->progress < req->length);

		size_t chunk = std::min(req->length - req->progress, fifoAvailable);
		assert(chunk);
		for(size_t i = 0; i < chunk; i++) {
			auto p = reinterpret_cast<const std::byte *>(req->buffer);
			base.store(uart_register::data, static_cast<uint8_t>(p[req->progress + i]));
		}
		req->progress += chunk;
		fifoAvailable -= chunk;

		// We only complete writes once we have written all bytes;
		// this avoids unnecessary round trips between the UART driver and the application.
		if(req->progress == req->length) {
			sendRequests.pop_front();
			pending.push_back(*req);
		}else{
			assert(!fifoAvailable); // In other words: we will exit the loop.
		}
	}

	if(logTx)
		std::cout << "uart: TX now in-flight" << std::endl;
	txInFlight = true;

	// Make sure that we set txInFlight before continuing asynchronous code.
	while(!pending.empty()) {
		auto req = &pending.front();
		pending.pop_front();
		req->promise.set_value();
	}
}

async::detached handleIrqs() {
	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();
		if(logIrqs)
			std::cout << "uart: IRQ fired." << std::endl;
		
		// The 8250's status register always reports the reason for one IRQ at a time.
		// Drain IRQs until the IRQ status register does not report any IRQs anymore.
		while(true) {
			auto reason = base.load(uart_register::irqIdentification);

			// Strangely, there is *no* pending IRQ from this device if the bit is *set*.
			if(reason & irq_ident_register::ignore)
				break;

			switch(reason & irq_ident_register::id) {
			case IrqIds::lineStatus:
				std::cout << "uart: Overrun, Parity, Framing or Break Error!" << std::endl;
				break;
			case IrqIds::dataAvailable:
			case IrqIds::charTimeout:
				if(logIrqs)
					std::cout << "uart: IRQ caused by: RX available" << std::endl;

				while(base.load(uart_register::lineStatus) & line_status::dataReady) {
					auto c = base.load(uart_register::data);
					recvBuffer.push_back(c);
				}
				if(!recvRequests.empty())
					completeRecvs();
				break;
			case IrqIds::txEmpty:
				if(logIrqs)
					std::cout << "uart: IRQ caused by: TX empty" << std::endl;

				if(txInFlight) {
					txInFlight = false;
					if(logTx)
						std::cout << "uart: TX not in-flight anymore" << std::endl;

					if(!sendRequests.empty())
						flushSends();
				}
				break;
			case IrqIds::modem:
				std::cout << "uart: Modem detected!" << std::endl;
				break;
			default:;
			}
		}

		// The 8250's interrupt model is broken, for example:
		// * RX available IRQs are cleared by reading the RX data register,
		// * TX empty IRQs are cleared by writing to the TX data register.
		// Hence, if we UART reports no pending IRQ, it might have happened that
		// we cleared the TX empty IRQ by writing additional bytes (similar for RX).
		// To be safe, we always acknowledge IRQs here.
		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, sequence));
	}
}
	
async::result<protocols::fs::ReadResult>
read(void *, const char *, void *buffer, size_t length) {
	if(!length)
		co_return size_t{0};

	ReadRequest req{buffer, length};
	recvRequests.push_back(req);

	if(!recvBuffer.empty())
		completeRecvs();

	co_await req.promise.async_get();
	co_return req.progress;
}

async::result<frg::expected<protocols::fs::Error, size_t>>
write(void *, const char *, const void *buffer, size_t length) {
	if(!length)
		co_return 0;

	if(logTx)
		std::cout << "uart: New TX request" << std::endl;

	WriteRequest req{buffer, length};
	sendRequests.push_back(req);

	if(!txInFlight)
		flushSends();

	co_await req.promise.async_get();

	if(logTx)
		std::cout << "uart: TX request done" << std::endl;
	co_return length;
}

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&read)
	.withWrite(&write);

async::detached serveTerminal(helix::UniqueLane lane) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), nullptr, &fileOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid serveTerminal request!");
		}
	}
}
	
async::detached runTerminal() {
	// Create an mbus object for the partition.
	auto root = co_await mbus::Instance::global().getRoot();
	
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

	co_await root.createObject("uart0", descriptor, std::move(handler));
}

int main() {
	std::cout << "uart: Starting driver" << std::endl;

	HelHandle irq_handle;
	HEL_CHECK(helAccessIrq(4, &irq_handle));
	irq = helix::UniqueIrq(irq_handle);

	uintptr_t ports[] = { COM1, COM1 + 1, COM1 + 2, COM1 + 3, COM1 + 4, COM1 + 5, COM1 + 6,
			COM1 + 7 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 8, &handle));
	HEL_CHECK(helEnableIo(handle));
	
	base = arch::global_io.subspace(COM1);

	// Perform general initialization.
	base.store(uart_register::fifoControl,
			fifo_control::fifoEnable(FifoCtrl::enable)
			| fifo_control::fifoIrqLvl(FifoCtrl::triggerLvl14));
	
	// Wait for the FIFO to become empty.
	while(!(base.load(uart_register::lineStatus) & line_status::txReady))
		; // Busy spin for now.

	// Enable IRQs.
	base.store(uart_register::irqEnable,
			irq_enable::dataAvailable(IrqCtrl::enable)
			| irq_enable::txEmpty(IrqCtrl::enable)
			| irq_enable::lineStatus(IrqCtrl::enable));

	// Set the baud rate.
	base.store(uart_register::lineControl, line_control::dlab(true));
	base.store(uart_register::baudLow, BaudRate::low9600);
	base.store(uart_register::baudHigh, BaudRate::high9600);

	base.store(uart_register::lineControl,
			line_control::dataBits(DataBits::charLen8)
			| line_control::stopBit(StopBits::one)
			| line_control::parityBits(Parity::none)
			| line_control::dlab(false));

	{
		async::queue_scope scope{helix::globalQueue()};
		runTerminal();
		handleIrqs();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);
	
	return 0;
}

