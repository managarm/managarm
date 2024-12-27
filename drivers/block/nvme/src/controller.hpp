#pragma once

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>

#include "queue.hpp"
#include "namespace.hpp"
#include "spec.hpp"

enum class ControllerType {
	PciExpress,
	FabricsTcp,
};

struct Controller {
	Controller(int64_t parentId, ControllerType type) : parentId_{parentId}, type_{type} {}
	virtual ~Controller() = default;

	virtual async::detached run() = 0;

	virtual async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) = 0;

	inline int64_t getParentId() const {
		return parentId_;
	}

	inline ControllerType getType() const {
		return type_;
	}

	async::result<void> scanNamespaces();

	async::result<Command::Result> identifyController(spec::IdentifyController &id);
	async::result<Command::Result> identifyNamespaceList(unsigned int nsid, arch::dma_buffer_view list);
	async::result<Command::Result> identifyNamespace(unsigned int nsid, spec::IdentifyNamespace &id);

	async::result<void> createNamespace(unsigned int nsid);

	spec::DataTransfer dataTransferPolicy() const {
		return preferredDataTransfer_;
	}

protected:
	spec::DataTransfer preferredDataTransfer_ = spec::DataTransfer::PRP;

	int64_t parentId_;
	uint32_t version_;
	const ControllerType type_;

	std::vector<std::unique_ptr<Queue>> activeQueues_;
	std::vector<std::unique_ptr<Namespace>> activeNamespaces_;
};

struct PciExpressController final : public Controller {
	PciExpressController(int64_t parentId, protocols::hw::Device hwDevice, helix::Mapping regsMapping,
			   helix::UniqueDescriptor irq);

	async::detached run() override;

	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) override;
private:
	static constexpr int IO_QUEUE_DEPTH = 1024;

	protocols::hw::Device hwDevice_;
	helix::Mapping regsMapping_;
	arch::mem_space regs_;
	helix::UniqueDescriptor irq_;

	unsigned int queueDepth_;
	uint32_t dbStride_;

	uint64_t irqSequence_;

	async::result<void> reset();

	async::result<void> waitStatus(bool enabled);
	async::result<void> enable();
	async::result<void> disable();

	async::result<bool> setupIoQueue(PciExpressQueue *q);
	async::result<Command::Result> createCQ(PciExpressQueue *q);
	async::result<Command::Result> createSQ(PciExpressQueue *q);

	async::detached handleIrqs();
};
