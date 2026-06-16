#pragma once

#include <async/oneshot-event.hpp>
#include <arch/dma_pool.hpp>

#include "spec.hpp"

enum class CommandType {
	read,
	write,
	identify
};

class Controller;

struct Command {
public:
	Command(Controller *controller, uint64_t sector, size_t numSectors, arch::dma_buffer_view view, CommandType type);
	Command() = delete;
	Command(Command&) = delete;
	Command& operator=(Command &) = delete;

	Command(Controller *controller, arch::dma_object_view<identifyDevice> buffer, CommandType type)
		: Command(controller, 0, 0, buffer.view_buffer(), type) {
		assert(type == CommandType::identify);
	}

	async::result<void> prepare(arch::dma_object_view<commandTable> table, commandHeader& header);
	void notifyCompletion();

	auto getFuture() {
		return event_.wait();
	}

private:
	async::result<size_t> writeScatterGather_(arch::dma_object_view<commandTable> table);

	Controller *controller_;

	uint64_t sector_;
	size_t numSectors_;
	size_t numBytes_;
	arch::dma_buffer_view view_;
	CommandType type_;
	async::oneshot_primitive event_;
};

constexpr const char *cmdTypeToString(CommandType type) {
	switch (type) {
		case CommandType::read:
			return "read";
		case CommandType::write:
			return "write";
		case CommandType::identify:
			return "identify";
		default:
			assert(!"unknown command type");
	}
}

