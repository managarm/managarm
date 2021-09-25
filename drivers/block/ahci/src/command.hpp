#pragma once

#include <async/oneshot-event.hpp>

#include "spec.hpp"

enum class CommandType {
	read,
	write,
	identify
};

struct Command {
public:
	Command(uint64_t sector, size_t numSectors, size_t numBytes, void *buffer, CommandType type);
	Command() = delete;
	Command(Command&) = delete;
	Command& operator=(Command &) = delete;

	Command(identifyDevice *buffer, CommandType type)
		: Command(0, 0, sizeof(identifyDevice), reinterpret_cast<void *>(buffer), type) {
		assert(type == CommandType::identify);
	}

	void prepare(commandTable& table, commandHeader& header);
	void notifyCompletion(); 

	auto getFuture() {
		return event_.wait();
	}

private:
	size_t writeScatterGather_(commandTable& table);

private:
	uint64_t sector_;
	size_t numSectors_;
	size_t numBytes_;
	void *buffer_;
	CommandType type_;
	async::oneshot_event event_;
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

