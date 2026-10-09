#pragma once

#include <async/mutex.hpp>
#include <netinet/in.h>
#include <optional>
#include <protocols/fs/client.hpp>
#include <protocols/mbus/client.hpp>
#include <span>

#include "../controller.hpp"

namespace nvme::fabric {

struct Tcp;

struct TcpQueue final : public Queue {
	TcpQueue(
		Tcp *controller,
	    uint16_t cid,
	    unsigned int index,
	    unsigned int depth,
	    in_addr addr,
	    in_port_t port,
	    helix::BorrowedLane lane,
	    std::span<uint8_t, 16> uuid
	);

	async::result<void> init() override;
	async::detached run() override;

	async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) override;

	uint16_t controllerId() {
		return controllerId_;
	}

	bool broken() const {
		return broken_;
	}

	void setInCapsuleDataSize(size_t size);
private:
	async::result<protocols::fs::Error> connect();
	async::detached keepAlive();
	async::detached submitPendingLoop();
	// Transfer exactly size bytes; fail the queue on errors and when the connection closes.
	async::result<protocols::fs::Error> receiveExact(void *buffer, size_t size);
	async::result<protocols::fs::Error> sendExact(const void *buffer, size_t size);
	async::result<void> submitCommandToDevice(std::unique_ptr<Command> cmd);
	async::detached sendH2CData(uint16_t slot, uint16_t transferTag, uint32_t offset, uint32_t length);
	void finishTransfer(uint16_t slot);
	// Fails all commands once the stream cannot be trusted anymore; NVMe/TCP cannot resynchronize.
	void fail();
	void resolveSlot(size_t slot, Command::Result result);
	void completeSlot(size_t slot, Command::Result result);

	in_addr addr_;
	in_port_t port_;
	helix::BorrowedLane lane_;
	uint16_t controllerId_;
	// keepalive timeout value in ms
	size_t keepAliveTimeout_ = 10'000;
	std::span<uint8_t, 16> uuid_;

	// Grows to the largest command capsule that has been sent.
	std::vector<std::byte> buf_;
	uint32_t maxH2CData_ = 0;
	size_t inCapsuleDataSize_ = 0;

	// Transport state of a slot that is not part of the command itself.
	struct SlotState {
		// Data transfers that still access the command's buffer.
		unsigned int activeTransfers = 0;
		// The response, if it arrived while data transfers were still active.
		std::optional<Command::Result> deferredResult;
		// Whether the controller fetches the command's data through R2T, i.e., a write without in-capsule data.
		bool r2tData = false;
	};
	std::vector<SlotState> slots_;

	async::oneshot_event connectedEvent_;
	bool broken_ = false;

	std::unique_ptr<protocols::fs::File> file_;

	async::mutex sendMutex;
};

struct Tcp final : public Controller {
	Tcp(mbus_ng::EntityId entity, in_addr addr, in_port_t port, std::string location, helix::UniqueLane netserver);

	async::detached run(mbus_ng::EntityId subsystem) override;
	async::result<Command::Result> submitAdminCommand(std::unique_ptr<Command> cmd) override;
	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) override;

	async::result<void> ensureMapped(arch::dma_buffer_view) override {
		// PRPs are not supported.
		co_return;
	}

	std::optional<uintptr_t> prpAddressOf(arch::dma_buffer_view) override {
		// PRPs are not supported.
		return std::nullopt;
	}

private:
	async::result<frg::expected<spec::CompletionStatus, uint64_t>> fabricGetProperty(uint32_t propertyOffset, size_t size);
	async::result<frg::expected<spec::CompletionStatus, uint64_t>> fabricSetProperty(uint32_t propertyOffset, uint64_t value, size_t size);

	in_addr serverAddr_;
	in_port_t serverPort_;
	helix::UniqueLane netserverLane_;
};

} // namespace nvme::fabric
