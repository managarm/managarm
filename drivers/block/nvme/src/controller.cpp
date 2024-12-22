#include <arch/bit.hpp>
#include <helix/timer.hpp>

#include "controller.hpp"

namespace regs {
	constexpr arch::bit_register<uint64_t> cap{0x0};
	constexpr arch::scalar_register<uint32_t> vs{0x8};
	constexpr arch::scalar_register<uint32_t> intms{0xc};
	constexpr arch::scalar_register<uint32_t> intmc{0x10};
	constexpr arch::bit_register<uint32_t> cc{0x14};
	constexpr arch::scalar_register<uint32_t> csts{0x1c};
	constexpr arch::scalar_register<uint32_t> aqa{0x24};
	constexpr arch::scalar_register<uint64_t> asq{0x28};
	constexpr arch::scalar_register<uint64_t> acq{0x30};
} // namespace regs

namespace flags {
	namespace cap {
		constexpr arch::field<uint64_t, uint16_t> mqes{0, 16};
		constexpr arch::field<uint64_t, uint8_t> dstrd{32, 4};
	} // namespace cap

	namespace vs {
		static inline uint32_t version(uint16_t major, uint8_t minor, uint8_t tertiary) {
			return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | tertiary;
		}
	} // namespace vs

	namespace cc {
		constexpr arch::field<uint32_t, uint8_t> iocqes{20, 4};
		constexpr arch::field<uint32_t, uint8_t> iosqes{16, 4};
		constexpr arch::field<uint32_t, bool> enable{0, 1};
	} // namespace cc

	namespace csts {
		constexpr int ready = 1 << 0;
	} // namespace csts
} // namespace flags

PciExpressController::PciExpressController(int64_t parentId, protocols::hw::Device hwDevice, helix::Mapping hbaRegs,
					   helix::UniqueDescriptor, helix::UniqueDescriptor irq)
	: Controller(parentId), hwDevice_{std::move(hwDevice)}, regsMapping_{std::move(hbaRegs)},
	  regs_{regsMapping_.get()}, irq_{std::move(irq)} {
}

async::detached PciExpressController::run() {
	co_await hwDevice_.enableBusIrq();

	handleIrqs();

	co_await reset();
	co_await scanNamespaces();

	for (auto &ns : activeNamespaces_)
		ns->run();
}

async::detached PciExpressController::handleIrqs() {
	irqSequence_ = 0;

	while (true) {
		auto awaitResult = co_await helix_ng::awaitEvent(irq_, irqSequence_);

		regs_.store(regs::intms, 1);

		HEL_CHECK(awaitResult.error());
		irqSequence_ = awaitResult.sequence();

		int found = 0;
		for (auto &q : activeQueues_) {
			found |= static_cast<PciExpressQueue *>(q.get())->handleIrq();
		}

		regs_.store(regs::intmc, 1);

		if (found) {
			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, irqSequence_));
		} else {
			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckNack, irqSequence_));
		}
	}
}

async::result<void> PciExpressController::waitStatus(bool enabled) {
	auto readyBit = enabled ? flags::csts::ready : 0;

	co_await helix::kindaBusyWait(50'000'000,
								  [&] { return (regs_.load(regs::csts) & flags::csts::ready) == readyBit; });
}

async::result<void> PciExpressController::enable() {
	auto cc = regs_.load(regs::cc);
	auto new_val = cc / flags::cc::iosqes(6) / flags::cc::iocqes(4) / flags::cc::enable(true);
	regs_.store(regs::cc, new_val);

	co_await waitStatus(true);
}

async::result<void> PciExpressController::disable() {
	auto cc = regs_.load(regs::cc);
	auto new_val = cc / flags::cc::enable(false);
	regs_.store(regs::cc, new_val);

	co_await waitStatus(false);
}

async::result<void> PciExpressController::reset() {
	auto cap = regs_.load(regs::cap);
	const auto doorbellsOffset = 0x1000;

	queueDepth_ = std::min((cap & flags::cap::mqes) + 1, IO_QUEUE_DEPTH);
	dbStride_ = 1 << (cap & flags::cap::dstrd);

	version_ = regs_.load(regs::vs);

	co_await disable();

	auto adminQ = std::make_unique<PciExpressQueue>(0, 32, regs_.subspace(doorbellsOffset));
	co_await adminQ->init();

	uint32_t aqa = (31 << 16) | 31;
	regs_.store(regs::aqa, aqa);
	regs_.store(regs::asq, adminQ->getSqPhysAddr());
	regs_.store(regs::acq, adminQ->getCqPhysAddr());

	adminQ->run();
	activeQueues_.push_back(std::move(adminQ));

	co_await enable();

	auto ioQ = std::make_unique<PciExpressQueue>(1, queueDepth_, regs_.subspace(doorbellsOffset + 1 * 8 * dbStride_));
	co_await ioQ->init();

	if (co_await setupIoQueue(ioQ.get())) {
		ioQ->run();
		activeQueues_.push_back(std::move(ioQ));
	}

	assert(activeQueues_.size() >= 2 && "At least need one IO queue");
}

async::result<bool> PciExpressController::setupIoQueue(PciExpressQueue *q) {
	auto cqRes = co_await createCQ(q);
	if (!cqRes.first.successful())
		co_return false;

	auto sqRes = co_await createSQ(q);
	if (!sqRes.first.successful())
		co_return false;

	co_return true;
}

async::result<Command::Result> PciExpressController::createCQ(PciExpressQueue *q) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().createCQ;

	uint16_t flags = spec::kQueuePhysContig | spec::kCQIrqEnabled;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateCQ);
	cmdBuf.prp1 = convert_endian<endian::little, endian::native>((uint64_t)q->getCqPhysAddr());
	cmdBuf.cqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());
	cmdBuf.qSize = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueDepth() - 1);
	cmdBuf.cqFlags = convert_endian<endian::little, endian::native>((uint16_t)flags);
	cmdBuf.irqVector = 0; // TODO: set to MSI vector

	return adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> PciExpressController::createSQ(PciExpressQueue *q) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().createSQ;

	uint16_t flags = spec::kQueuePhysContig;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateSQ);
	cmdBuf.prp1 = convert_endian<endian::little, endian::native>((uint64_t)q->getSqPhysAddr());
	cmdBuf.sqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());
	cmdBuf.qSize = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueDepth() - 1);
	cmdBuf.sqFlags = convert_endian<endian::little, endian::native>((uint16_t)flags);
	cmdBuf.cqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());

	return adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyController(spec::IdentifyController &id) {
	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyController;
	cmd->setupBuffer(arch::dma_buffer_view{nullptr, &id, sizeof(id)}, preferredDataTransfer_);

	return adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyNamespaceList(unsigned int nsid, arch::dma_buffer_view list) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyActiveList;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid);
	cmd->setupBuffer(list, preferredDataTransfer_);

	return adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyNamespace(unsigned int nsid, spec::IdentifyNamespace &id) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyNamespace;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid);
	cmd->setupBuffer(arch::dma_buffer_view{nullptr, &id, sizeof(id)}, preferredDataTransfer_);

	return adminQ->submitCommand(std::move(cmd));
}

async::result<void> Controller::scanNamespaces() {
	using arch::convert_endian;
	using arch::endian;

	spec::IdentifyController idCtrl;
	int nn;

	if (!(co_await identifyController(idCtrl)).first.successful())
		co_return;

	nn = convert_endian<endian::little>(idCtrl.nn);

	if (version_ >= flags::vs::version(1, 1, 0)) {
		auto nsList = arch::dma_array<uint32_t>{nullptr, 1024};
		int numLists = (nn + 1023) >> 10;
		unsigned int prev = 0;
		for (auto i = 0; i < numLists; i++) {
			if (!(co_await identifyNamespaceList(prev, nsList.view_buffer())).first.successful())
				co_return;

			int j;
			for (j = 0; j < std::min((int)nn, 1024); j++) {
				auto nsid = convert_endian<endian::little>(nsList[j]);
				if (!nsid)
					co_return;

				co_await createNamespace(nsid);

				prev = nsid;
			}

			nn -= j;
		}

		co_return;
	}

	for (int i = 1; i <= nn; i++) {
		co_await createNamespace(i);
	}
}

async::result<void> Controller::createNamespace(unsigned int nsid) {
	spec::IdentifyNamespace id;

	if (!(co_await identifyNamespace(nsid, id)).first.successful())
		co_return;

	if (!id.ncap)
		co_return;

	auto lbaShift = id.lbaf[id.flbas & 0xf].ds;
	if (!lbaShift)
		lbaShift = 9;

	auto ns = std::make_unique<Namespace>(this, nsid, lbaShift);
	activeNamespaces_.push_back(std::move(ns));
}

async::result<Command::Result> PciExpressController::submitIoCommand(std::unique_ptr<Command> cmd) {
	auto &ioQ = activeQueues_.back();

	return ioQ->submitCommand(std::move(cmd));
}
