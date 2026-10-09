#include <format>
#include <helix/timer.hpp>
#include <protocols/fs/client.hpp>
#include <sys/epoll.h>

#include "../controller.hpp"
#include "tcp.hpp"

namespace nvme::fabric {

namespace {

// All PDUs that TcpQueue::run() handles have a header of this size.
constexpr size_t pduHeaderSize = sizeof(spec::tcp::CapsuleResp);
static_assert(sizeof(spec::tcp::R2T) == pduHeaderSize);
static_assert(sizeof(spec::tcp::C2HData) == pduHeaderSize);

// Host Pathing Error, which Linux also reports for commands on a failed transport.
constexpr Command::Result hostPathError{spec::CompletionStatus{(0x3 << 9) | (0x70 << 1)}, {}};

} // namespace

TcpQueue::TcpQueue(Tcp *controller, uint16_t cid, unsigned int index, unsigned int depth, in_addr addr, in_port_t port, helix::BorrowedLane lane, std::span<uint8_t, 16> uuid)
: Queue(controller, index, depth), addr_{addr}, port_{port}, lane_{std::move(lane)}, controllerId_{cid}, uuid_{uuid} {
	slots_.resize(depth);
}

void TcpQueue::setInCapsuleDataSize(size_t size) {
	inCapsuleDataSize_ = size;
}

async::result<protocols::fs::Error> TcpQueue::connect() {
	spec::tcp::ICReq connect_req {
		.ch{
			.pduType = spec::tcp::PduType::ICReq,
			.flags = 0,
			.headerLength = sizeof(spec::tcp::ICReq),
			.pduDataOffset = 0,
			.pduLength = sizeof(spec::tcp::ICReq),
		},
		.pduFormatVersion = 0,
		.hostPduDataAlignment = 0,
		.digest = 0,
		.maxr2t = 0,
	};

	if(auto e = co_await sendExact(&connect_req, sizeof(connect_req)); e != protocols::fs::Error::none)
		co_return e;

	spec::tcp::ICResp resp{};
	if(auto e = co_await receiveExact(&resp, sizeof(resp)); e != protocols::fs::Error::none)
		co_return e;

	if(resp.ch.pduType != spec::tcp::PduType::ICResp)
		co_return protocols::fs::Error::addressNotAvailable;

	// We place PDU data right after the header and do not pad it to the controller's alignment.
	if(resp.controllerPduDataAlignment) {
		std::cout << std::format("block/nvme: NVMe/TCP controller requires unsupported PDU data alignment {}",
				resp.controllerPduDataAlignment) << std::endl;
		co_return protocols::fs::Error::notSupported;
	}

	maxH2CData_ = resp.maxh2cdata;

	connectedEvent_.raise();

	co_return protocols::fs::Error::none;
}

async::result<void> TcpQueue::init() {
	auto sock_err = co_await protocols::fs::File::createSocket(lane_, AF_INET, SOCK_STREAM, 0, 0);
	if(!sock_err) {
		std::cout << "block/nvme: failed to create socket for queue " << qid_ << std::endl;
		fail();
		co_return;
	}

	file_ = std::make_unique<protocols::fs::File>(std::move(sock_err.value()));

	sockaddr_in sockaddr{};
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(port_);
	sockaddr.sin_addr.s_addr = addr_.s_addr;

	auto connect_err = co_await file_->connect(reinterpret_cast<struct sockaddr *>(&sockaddr), sizeof(sockaddr));
	if(connect_err != protocols::fs::Error::none) {
		std::cout << "block/nvme: failed to TCP connect for queue " << qid_ << std::endl;
		fail();
		co_return;
	}

	if(co_await connect() != protocols::fs::Error::none) {
		std::cout << "block/nvme: failed to init queue " << qid_ << std::endl;
		fail();
		co_return;
	}

	auto cmd = std::make_unique<Command>();
	auto &connectCmd = cmd->getCommandBuffer().fabricConnect;
	connectCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	connectCmd.flags = 0x40;
	connectCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::Connect);
	connectCmd.recordFormat = 0;
	connectCmd.queueId = qid_;
	connectCmd.sqSize = depth_ - 1;
	connectCmd.connectAttrs = 0;
	connectCmd.keepAliveTimeout = keepAliveTimeout_;

	spec::fabric::ConnectCommandData connectData({
		.controllerId = controllerId_,
		.subsystemNqn = "nqn.2024-12.org.managarm:nvme:managarm-boot",
	});

	memcpy(connectData.hostIdentifier, uuid_.data(), sizeof(connectData.hostIdentifier));
	auto nqn = std::format("nqn.2014-08.org.nvmexpress:uuid:{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		uuid_[0], uuid_[1], uuid_[2], uuid_[3], uuid_[4], uuid_[5], uuid_[6], uuid_[7],
		uuid_[8], uuid_[9], uuid_[10], uuid_[11], uuid_[12], uuid_[13], uuid_[14], uuid_[15]);
	strncpy(connectData.hostNqn, nqn.c_str(), sizeof(connectData.hostNqn));

	co_await cmd->setupBuffer(controller_, arch::dma_buffer_view{nullptr, &connectData, sizeof(connectData)}, spec::DataTransfer::SGL);
	auto res = co_await submitCommand(std::move(cmd));
	if(!res.first.successful()) {
		std::cout << "block/nvme: failed to set up queue " << qid_ << std::endl;
		fail();
		co_return;
	}

	controllerId_ = res.second.u16;
}

async::detached TcpQueue::keepAlive() {
	// sane keepalive timeout values are between 5 sec and 10 min
	assert(keepAliveTimeout_ > 5000);
	assert(keepAliveTimeout_ < 10 * 60 * 1000);

	while(!broken_) {
		// Like Linux, send at half the timeout to avoid hitting the timeout due to lag.
		co_await helix::sleepFor(keepAliveTimeout_ / 2 * 1'000'000);

		auto cmd = std::make_unique<Command>();
		auto &packet = cmd->getCommandBuffer().common;
		packet.opcode = static_cast<uint8_t>(spec::AdminOpcode::KeepAlive);
		co_await cmd->setupBuffer(controller_, arch::dma_buffer_view{}, spec::DataTransfer::SGL);
		co_await submitCommand(std::move(cmd));
	}
}

async::detached TcpQueue::run() {
	co_await connectedEvent_.wait();

	submitPendingLoop();

	if(qid_ == 0)
		keepAlive();

	alignas(spec::tcp::C2HData) std::byte header[pduHeaderSize];

	while(true) {
		if(co_await receiveExact(header, pduHeaderSize) != protocols::fs::Error::none)
			co_return;

		auto ch = reinterpret_cast<spec::tcp::PduCommonHeader *>(header);

		// Only C2HData carries data, which is received directly into the command's buffer.
		if(ch->headerLength != pduHeaderSize || ch->pduLength < pduHeaderSize
				|| (ch->pduType != spec::tcp::PduType::C2HData && ch->pduLength != pduHeaderSize)) {
			std::cout << std::format("block/nvme: NVMe-oF PDU of type {:#x} is malformed", static_cast<uint8_t>(ch->pduType)) << std::endl;
			fail();
			co_return;
		}

		switch(ch->pduType) {
			case spec::tcp::PduType::CapsuleResp: {
				auto capsuleResp = reinterpret_cast<spec::tcp::CapsuleResp *>(header);
				auto slot = capsuleResp->responseCqe.commandId;
				if(slot < queuedCmds_.size() && queuedCmds_[slot])
					resolveSlot(slot, {spec::CompletionStatus{capsuleResp->responseCqe.status}, capsuleResp->responseCqe.result});
				break;
			}
			case spec::tcp::PduType::R2T: {
				auto r2t = reinterpret_cast<spec::tcp::R2T *>(header);
				auto slot = r2t->commandCapsuleId;
				if(slot >= queuedCmds_.size() || !queuedCmds_[slot] || slots_[slot].deferredResult) {
					std::cout << std::format("block/nvme: R2T for unknown command {}", slot) << std::endl;
					fail();
					co_return;
				}
				if(!slots_[slot].r2tData) {
					std::cout << std::format("block/nvme: Unexpected R2T for command {} that is not a write without in-capsule data", slot) << std::endl;
					fail();
					co_return;
				}
				// We would send no H2CData PDU at all, so the controller would never see the last one.
				if(!r2t->r2tLength) {
					std::cout << std::format("block/nvme: R2T of zero length for command {}", slot) << std::endl;
					fail();
					co_return;
				}
				auto &view = queuedCmds_[slot]->view();
				if(!view.byte_data() || uint64_t{r2t->r2tOffset} + r2t->r2tLength > view.size()) {
					std::cout << std::format("block/nvme: R2T exceeds the data of command {}", slot) << std::endl;
					fail();
					co_return;
				}
				slots_[slot].activeTransfers++;
				sendH2CData(slot, r2t->transferTag, r2t->r2tOffset, r2t->r2tLength);
				break;
			}
			case spec::tcp::PduType::C2HData: {
				auto resp = reinterpret_cast<spec::tcp::C2HData *>(header);

				// We negotiate neither digests nor padding, so the data makes up the rest of the PDU.
				if(resp->dataLength != resp->ch.pduLength - pduHeaderSize
						|| (resp->dataLength && resp->ch.pduDataOffset != pduHeaderSize)) {
					std::cout << std::format("block/nvme: NVMe-oF C2HData PDU is malformed") << std::endl;
					fail();
					co_return;
				}

				auto slot = resp->commandCapsuleId;
				if(slot >= queuedCmds_.size() || !queuedCmds_[slot]) {
					std::cout << std::format("block/nvme: C2HData for unknown command {}", slot) << std::endl;
					fail();
					co_return;
				}
				// Like Linux, reject C2HData for writes, i.e., opcodes with bit 0 set (which includes Fabrics).
				if(queuedCmds_[slot]->getCommandBuffer().common.opcode & 1) {
					std::cout << std::format("block/nvme: Unexpected C2HData for write command {}", slot) << std::endl;
					fail();
					co_return;
				}

				auto &view = queuedCmds_[slot]->view();
				if(!view.byte_data() || uint64_t{resp->dataOffset} + resp->dataLength > view.size()) {
					std::cout << std::format("block/nvme: C2HData exceeds the data of command {}", slot) << std::endl;
					fail();
					co_return;
				}

				// fail() on a concurrent send must not hand the buffer back while we receive into it.
				slots_[slot].activeTransfers++;
				auto recv_err = co_await receiveExact(view.byte_data() + resp->dataOffset, resp->dataLength);
				finishTransfer(slot);
				if(recv_err != protocols::fs::Error::none)
					co_return;
				break;
			}
			default: {
				std::cout << std::format("block/nvme: unhandled NVMe-oF PDU type {:#x}", static_cast<uint8_t>(ch->pduType)) << std::endl;
				fail();
				co_return;
			}
		}
	}
}

async::result<protocols::fs::Error> TcpQueue::receiveExact(void *buffer, size_t size) {
	size_t received = 0;
	while(received < size) {
		auto recv_err = co_await file_->recvfrom(static_cast<std::byte *>(buffer) + received, size - received, 0, nullptr, 0);
		if(!recv_err) {
			std::cout << "block/nvme: error on receive for queue " << qid_ << std::endl;
			fail();
			co_return recv_err.error();
		}
		if(!recv_err.value()) {
			std::cout << "block/nvme: connection of queue " << qid_ << " was closed" << std::endl;
			fail();
			co_return protocols::fs::Error::endOfFile;
		}
		received += recv_err.value();
	}
	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> TcpQueue::sendExact(const void *buffer, size_t size) {
	size_t sent = 0;
	while(sent < size) {
		auto send_err = co_await file_->sendto(static_cast<const std::byte *>(buffer) + sent, size - sent, 0, nullptr, 0);
		if(!send_err) {
			std::cout << "block/nvme: error on send for queue " << qid_ << std::endl;
			fail();
			co_return send_err.error();
		}
		sent += send_err.value();
	}
	co_return protocols::fs::Error::none;
}

async::detached TcpQueue::submitPendingLoop() {
	while (true) {
		auto cmd = co_await pendingCmdQueue_.async_get();
		assert(cmd);
		co_await submitCommandToDevice(std::move(cmd.value()));
	}
}

async::result<void> TcpQueue::submitCommandToDevice(std::unique_ptr<Command> cmd) {
	auto slot = co_await findFreeSlot();

	if(broken_) {
		cmd->complete(hostPathError.first, hostPathError.second);
		co_return;
	}

	// we can safely reuse the buffer as we are (implicitly) serialized by `submitPendingLoop`
	auto data_len = cmd->view().size();

	// Fabrics commands (i.e., Connect) always carry their data in the capsule.
	// Like Linux, send write data in-capsule whenever it fits.
	// Besides saving a round trip, Linux' nvmet target may not send an R2T for a write that it expects in-capsule data for.
	auto opcode = cmd->getCommandBuffer().common.opcode;
	bool inCapsule = data_len && (opcode == static_cast<uint8_t>(spec::AdminOpcode::Fabrics)
			|| ((opcode & 1) && data_len <= inCapsuleDataSize_));

	// Grow the buffer to the largest capsule actually sent, before taking pointers into it.
	size_t dataOffset = sizeof(spec::tcp::CapsuleCmd) + sizeof(cmd->getCommandBuffer());
	size_t capsuleSize = dataOffset + (inCapsule ? data_len : 0);
	if(buf_.size() < capsuleSize)
		buf_.resize(capsuleSize);

	new (buf_.data()) spec::tcp::CapsuleCmd({
		.ch = {
			.pduType = spec::tcp::PduType::CapsuleCmd,
			.flags = 0,
			.headerLength = sizeof(spec::tcp::CapsuleCmd) + sizeof(cmd->getCommandBuffer()),
			.pduDataOffset = 0,
			.pduLength = sizeof(spec::tcp::CapsuleCmd),
		}
	});

	auto capsuleCmd = reinterpret_cast<spec::tcp::CapsuleCmd *>(buf_.data());
	capsuleCmd->ch.pduLength += sizeof(cmd->getCommandBuffer());

	memcpy(&buf_[sizeof(spec::tcp::CapsuleCmd)], &cmd->getCommandBuffer(), sizeof(cmd->getCommandBuffer()));

	auto genericCommand = reinterpret_cast<spec::Command *>(&buf_[sizeof(spec::tcp::CapsuleCmd)]);
	genericCommand->common.commandId = slot;

	if(inCapsule) {
		// An SGL data block descriptor of subtype offset denotes in-capsule data.
		auto &sgl = genericCommand->common.dataPtr.sgl.generic;
		sgl.sglDescriptorType = 0;
		sgl.sglSubType = 1;

		capsuleCmd->ch.pduDataOffset = dataOffset;
		memcpy(&buf_[dataOffset], reinterpret_cast<void *>(cmd->view().byte_data()), data_len);
		capsuleCmd->ch.pduLength += data_len;
	}

	slots_[slot].r2tData = data_len && (opcode & 1) && !inCapsule;
	queuedCmds_[slot] = std::move(cmd);
	commandsInFlight_++;

	// H2C data PDUs are sent concurrently; PDUs must not interleave on the stream.
	co_await sendMutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, sendMutex};

	// fail() has already completed the command.
	if(broken_)
		co_return;

	co_await sendExact(buf_.data(), capsuleCmd->ch.pduLength);
}

async::detached TcpQueue::sendH2CData(uint16_t slot, uint16_t transferTag, uint32_t offset, uint32_t length) {
	// The command cannot complete before finishTransfer(), so its buffer stays valid.
	auto data = queuedCmds_[slot]->view().byte_data();
	std::vector<std::byte> pdu;

	// MAXH2CDATA must be at least 4096; do not loop forever on a controller that reports 0.
	auto maxChunk = maxH2CData_ ? maxH2CData_ : length;

	for(uint32_t done = 0; done < length && !broken_;) {
		auto chunk = std::min<uint32_t>(length - done, maxChunk);
		spec::tcp::H2CData header{
			.ch = {
				.pduType = spec::tcp::PduType::H2CData,
				.flags = static_cast<uint8_t>(done + chunk == length ? spec::tcp::pduFlagDataLast : 0),
				.headerLength = sizeof(spec::tcp::H2CData),
				.pduDataOffset = sizeof(spec::tcp::H2CData),
				.pduLength = static_cast<uint32_t>(sizeof(spec::tcp::H2CData) + chunk),
			},
			.commandCapsuleId = slot,
			.transferTag = transferTag,
			.dataOffset = offset + done,
			.dataLength = chunk,
		};
		pdu.resize(sizeof(header) + chunk);
		memcpy(pdu.data(), &header, sizeof(header));
		memcpy(pdu.data() + sizeof(header), data + offset + done, chunk);

		co_await sendMutex.async_lock();
		frg::unique_lock lock{frg::adopt_lock, sendMutex};

		if(broken_)
			break;

		if(co_await sendExact(pdu.data(), pdu.size()) != protocols::fs::Error::none)
			break;

		done += chunk;
	}

	finishTransfer(slot);
}

void TcpQueue::finishTransfer(uint16_t slot) {
	auto &state = slots_[slot];
	assert(state.activeTransfers);
	if(!--state.activeTransfers && state.deferredResult)
		completeSlot(slot, *std::exchange(state.deferredResult, std::nullopt));
}

void TcpQueue::fail() {
	if(broken_)
		return;
	broken_ = true;
	std::cout << "block/nvme: NVMe/TCP queue " << qid_ << " is broken, failing all of its commands" << std::endl;

	// Fail commands that have not been submitted yet.
	while(auto cmd = pendingCmdQueue_.maybe_get())
		(*cmd)->complete(hostPathError.first, hostPathError.second);

	// Fail commands that are waiting for completion.
	for(size_t slot = 0; slot < queuedCmds_.size(); slot++) {
		// A deferred result is the controller's own and is delivered once its data transfers finish.
		if(queuedCmds_[slot] && !slots_[slot].deferredResult)
			resolveSlot(slot, hostPathError);
	}
}

void TcpQueue::resolveSlot(size_t slot, Command::Result result) {
	// Completing hands the buffer back to the caller, so wait until no transfer accesses it.
	if(slots_[slot].activeTransfers)
		slots_[slot].deferredResult = result;
	else
		completeSlot(slot, result);
}

void TcpQueue::completeSlot(size_t slot, Command::Result result) {
	auto cmd = std::move(queuedCmds_[slot]);
	cmd->complete(result.first, result.second);
	commandsInFlight_--;
	freeSlotDoorbell_.raise();
}

async::result<Command::Result> TcpQueue::submitCommand(std::unique_ptr<Command> cmd) {
	if(broken_)
		co_return hostPathError;

	auto future = cmd->getFuture();
	pendingCmdQueue_.put(std::move(cmd));
	co_return *(co_await future.get());
}

Tcp::Tcp(mbus_ng::EntityId entity, in_addr addr, in_port_t port, std::string location, helix::UniqueLane netserver)
: Controller(entity, location, ControllerType::FabricsTcp), serverAddr_{addr}, serverPort_{port}, netserverLane_{std::move(netserver)} {
	preferredDataTransfer_ = spec::DataTransfer::SGL;
}

async::detached Tcp::run(mbus_ng::EntityId subsystem) {
	uint8_t uuid[16];
	size_t n = 0;
	while(n < 16) {
		size_t chunk;
		HEL_CHECK(helGetRandomBytes(uuid + n, 16 - n, &chunk));
		n += chunk;
	}

	uuid[6] = (uuid[6] & 0x0F) | 0x40;
	uuid[8] = (uuid[8] & 0x3F) | 0x80;

	auto adminq = std::make_unique<TcpQueue>(this, 0xFFFF, 0, 32, serverAddr_, serverPort_, netserverLane_, std::span<uint8_t, 16>{uuid});
	// NVMe/TCP admin queue command capsules carry up to 8 KiB of in-capsule data after the SQE.
	adminq->setInCapsuleDataSize(8192);
	adminq->run();
	co_await adminq->init();
	auto cid = adminq->controllerId();
	bool adminqBroken = adminq->broken();
	// The queue's detached coroutines still reference it, so keep it alive even if it failed.
	activeQueues_.push_back(std::move(adminq));
	if(adminqBroken) {
		std::cout << "block/nvme: failed to set up the NVMe/TCP admin queue" << std::endl;
		co_return;
	}

	std::cout << std::format("block/nvme: TCP socket connected to controller") << std::endl;

	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-controller"}},
		{"nvme.subsystem", mbus_ng::StringItem{std::to_string(subsystem)}},
		{"nvme.address", mbus_ng::StringItem{location_}},
		{"nvme.transport", mbus_ng::StringItem{"tcp"}},
		{"nvme.serial", mbus_ng::StringItem{serial}},
		{"nvme.model", mbus_ng::StringItem{model}},
		{"nvme.fw-rev", mbus_ng::StringItem{fw_rev}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{"-1"}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-controller", descriptor)).unwrap());

	auto set_prop_err = co_await fabricSetProperty(0x14, 0x00460060, 4);
	if(!set_prop_err) {
		std::cout << "block/name: failed to configure Controller parameters" << std::endl;
		co_return;
	}

	set_prop_err = co_await fabricSetProperty(0x14, 0x00460061, 4);
	if(!set_prop_err) {
		std::cout << "block/name: failed to enable Controller" << std::endl;
		co_return;
	}

	auto cmd = std::make_unique<Command>();
	auto &setFeature = cmd->getCommandBuffer().setFeatures;
	setFeature.opcode = static_cast<uint8_t>(spec::AdminOpcode::SetFeatures);
	setFeature.nsid = 0;
	setFeature.data[0] = 0x07;
	setFeature.data[1] = 0;

	co_await cmd->setupBuffer(this, arch::dma_buffer_view{}, preferredDataTransfer_);
	co_await activeQueues_.front()->submitCommand(std::move(cmd));

	// This also identifies the controller, which yields IOCCSZ for the I/O queue.
	co_await scanNamespaces();

	// // setup I/O queue
	auto ioq = std::make_unique<TcpQueue>(this, cid, 1, 128, serverAddr_, serverPort_, netserverLane_, std::span<uint8_t, 16>{uuid});
	// IOCCSZ bounds the in-capsule data on I/O queues.
	size_t ioCapsuleSize = size_t{ioccsz_} * 16;
	if(!icdoff_ && ioCapsuleSize > sizeof(spec::Command))
		ioq->setInCapsuleDataSize(ioCapsuleSize - sizeof(spec::Command));
	ioq->run();
	co_await ioq->init();
	bool ioqBroken = ioq->broken();
	activeQueues_.push_back(std::move(ioq));
	if(ioqBroken) {
		std::cout << "block/nvme: failed to set up the NVMe/TCP I/O queue" << std::endl;
		co_return;
	}

	for (auto &ns : activeNamespaces_)
		ns->run();
}

async::result<Command::Result> Tcp::submitAdminCommand(std::unique_ptr<Command> cmd) {
	co_return co_await activeQueues_.at(0)->submitCommand(std::move(cmd));
}

async::result<Command::Result> Tcp::submitIoCommand(std::unique_ptr<Command> cmd) {
	co_return co_await activeQueues_.at(1)->submitCommand(std::move(cmd));
}

async::result<frg::expected<spec::CompletionStatus, uint64_t>> Tcp::fabricGetProperty(uint32_t propertyOffset, size_t size) {
	assert(size == 4 || size == 8);

	auto cmd = std::make_unique<Command>();
	auto &propCmd = cmd->getCommandBuffer().fabricPropertyGet;

	propCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	propCmd.flags = 0x40;
	propCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::PropertyGet);
	propCmd.attributes = (size == 4) ? 0 : 1;
	propCmd.offset = propertyOffset;

	auto res = co_await activeQueues_.front()->submitCommand(std::move(cmd));
	if(res.first.successful())
		co_return {res.second.u64};

	co_return spec::CompletionStatus{res.first};
}

async::result<frg::expected<spec::CompletionStatus, uint64_t>> Tcp::fabricSetProperty(uint32_t propertyOffset, uint64_t value, size_t size) {
	assert(size == 4 || size == 8);

	auto cmd = std::make_unique<Command>();
	auto &propCmd = cmd->getCommandBuffer().fabricPropertySet;

	propCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	propCmd.flags = 0x40;
	propCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::PropertySet);
	propCmd.attributes = (size == 4) ? 0 : 1;
	propCmd.offset = propertyOffset;
	propCmd.value = value;

	auto res = co_await activeQueues_.front()->submitCommand(std::move(cmd));
	if(res.first.successful())
		co_return {res.second.u64};

	co_return spec::CompletionStatus{res.first};
}

} // namespace nvme::fabric
