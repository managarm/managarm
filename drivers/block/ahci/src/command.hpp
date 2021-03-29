#pragma once

#include <async/result.hpp>

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

	Command(identify_device *buffer, CommandType type)
		: Command(0, 0, sizeof(identify_device), reinterpret_cast<void *>(buffer), type) {
		assert(type == CommandType::identify);
	}

	void prepare(command_table& table, command_header& header);
	void notifyCompletion(); 

	async::result<void> getFuture() {
		return promise_.async_get();
	}

private:
	size_t writeScatterGather_(command_table& table);

private:
	uint64_t sector_;
	size_t numSectors_;
	size_t numBytes_;
	void *buffer_;
	CommandType type_;
	async::promise<void> promise_;
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

