#pragma once

#include <queue>

#include <arch/mem_space.hpp>
#include <arch/dma_structs.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>

#include <blockfs.hpp>

#include "spec.hpp"
#include "command.hpp"

class Port : public blockfs::BlockDevice {
public:
	Port(int64_t parentId, int index, size_t numCommandSlots, bool staggeredSpinUp,
			arch::mem_space regs);

public:
	async::result<bool> init();
	async::detached run();
	void handleIrq();
	void dumpState();
	void checkErrors();

	async::result<void> readSectors(uint64_t sector, void *buf, size_t numSectors) override;
	async::result<void> writeSectors(uint64_t sector, const void *buf, size_t numSectors) override;
	async::result<size_t> getSize() override;

	int getIndex() const { return portIndex_; }

private:
	async::result<size_t> findFreeSlot_();
	async::detached submitPendingLoop_();
	async::result<void> submitCommand_(Command *cmd);
	void start_();
	void stop_();

private:
	// Mapping is owned by Controller
	arch::mem_space regs_;

	arch::dma_object<commandList> commandList_;
	arch::dma_array<commandTable> commandTables_;
	arch::dma_object<receivedFis> receivedFis_;

	// TODO: Move this to libasync
	struct stl_allocator {
		void *allocate(size_t size) {
			return operator new(size);
		}

		void deallocate(void *p, size_t) {
			return operator delete(p);
		}
	};
	async::queue<Command *, stl_allocator> pendingCmdQueue_;

	std::array<Command *, limits::maxCmdSlots> submittedCmds_{};
	async::recurring_event freeSlotDoorbell_;

	uint64_t deviceSize_;
	size_t numCommandSlots_;
	size_t commandsInFlight_;
	int portIndex_;
	bool staggeredSpinUp_;
};
