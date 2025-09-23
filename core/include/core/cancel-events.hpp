#pragma once

#include <expected>
#include <helix/ipc.hpp>
#include <map>

struct CancelEventRegistry {
	struct Error {};

	struct CancelEventGuard {
		friend CancelEventRegistry;

		CancelEventGuard(CancelEventRegistry *r, helix_ng::Credentials creds, uint64_t id)
		: r_{r}, creds_{creds}, id_{id} {
			token_ = r_->registerEvent(creds_, id_);
		}

		CancelEventGuard()
		: r_{nullptr}, creds_{}, id_{0}, token_{} { }

		CancelEventGuard(const CancelEventGuard &) = delete;

		CancelEventGuard(CancelEventGuard &&other) {
			swap(*this, other);
		}

		CancelEventGuard &operator=(CancelEventGuard other) {
			swap(*this, other);
			return *this;
		}

		~CancelEventGuard() {
			if (r_)
				r_->removeEvent(creds_, id_);
		}

		friend void swap(CancelEventGuard &a, CancelEventGuard &b) {
			using std::swap;
			swap(a.r_, b.r_);
			swap(a.creds_, b.creds_);
			swap(a.id_, b.id_);
			swap(a.token_, b.token_);
		}

		operator bool() const {
			return token_.has_value();
		}

		operator async::cancellation_token() {
			assert(token_.has_value());
			return token_.value();
		}

	private:
		CancelEventRegistry *r_;
		helix_ng::Credentials creds_;
		uint64_t id_;
		std::expected<async::cancellation_token, Error> token_;
	};

	CancelEventGuard event(helix_ng::Credentials creds, uint64_t id) {
		CancelEventGuard reg{this, creds, id};
		return reg;
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

protected:
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

private:
	// TODO: unordered_map would be better, but std::pair is not hashable ...
	std::map<std::pair<helix_ng::Credentials, uint64_t>, async::cancellation_event> list_;
};
