#pragma once

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>

#include "namespace.hpp"
#include "queue.hpp"

struct Controller {
	Controller(
	    int64_t parentId,
	    protocols::hw::Device hwDevice,
	    helix::Mapping hbaRegs,
	    helix::UniqueDescriptor ahciBar,
	    helix::UniqueDescriptor irq
	);

	async::detached run();

	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd);

	inline int64_t getParentId() const { return parentId_; }

  private:
	static constexpr int IO_QUEUE_DEPTH = 1024;

	protocols::hw::Device hwDevice_;
	helix::Mapping regsMapping_;
	arch::mem_space regs_;
	helix::UniqueDescriptor irq_;

	std::vector<std::unique_ptr<Queue>> activeQueues_;
	std::vector<std::unique_ptr<Namespace>> activeNamespaces_;

	int64_t parentId_;
	unsigned int queueDepth_;
	uint32_t dbStride_;
	uint32_t version_;

	uint64_t irqSequence_;

	async::result<void> reset();
	async::result<void> scanNamespaces();

	async::result<void> waitStatus(bool enabled);
	async::result<void> enable();
	async::result<void> disable();

	async::result<bool> setupIoQueue(Queue *q);
	async::result<Command::Result> createCQ(Queue *q);
	async::result<Command::Result> createSQ(Queue *q);

	async::result<Command::Result> identifyController(spec::IdentifyController &id);
	async::result<Command::Result> identifyNamespaceList(unsigned int nsid, uint32_t *list);
	async::result<Command::Result>
	identifyNamespace(unsigned int nsid, spec::IdentifyNamespace &id);

	async::result<void> createNamespace(unsigned int nsid);

	async::detached handleIrqs();
};
