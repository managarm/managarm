#pragma once

#include <expected>
#include <helix/ipc.hpp>
#include <map>

struct CancelEventRegistry {
	struct Error {};

	std::expected<async::cancellation_token, Error> registerEvent(helix_ng::Credentials creds, uint64_t id) {
		if (id == 0)
			return {};

		auto emplaceResult = list_.emplace(std::piecewise_construct,
			std::tuple{creds, id}, std::tuple{});
		if (!emplaceResult.second)
			return std::unexpected{Error{}};

		return {emplaceResult.first->second};
	}

	void removeEvent(helix_ng::Credentials creds, uint64_t id) {
		list_.erase({creds, id});
	}

	bool cancel(helix_ng::Credentials creds, uint64_t id) {
		auto res = list_.find({creds, id});

		if (res != list_.end()) {
			res->second.cancel();
			return true;
		} else {
			return false;
		}
	}

private:
	// TODO: unordered_map would be better, but std::pair is not hashable ...
	std::map<std::pair<helix_ng::Credentials, uint64_t>, async::cancellation_event> list_;
};
