#pragma once

#include <atomic>

#include <assert.h>
#include <frg/list.hpp>
#include <smarter.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>
#include <thor-internal/physical.hpp>

#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/asid.hpp>

namespace thor {

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);

struct KernelPageSpace : PageSpace {
	static void initialize();

	static KernelPageSpace &global();

	// TODO(qookie): This should be private, but the ctor is invoked by frigg
	explicit KernelPageSpace(PhysicalAddr ttbr1);

	KernelPageSpace(const KernelPageSpace &) = delete;

	KernelPageSpace &operator= (const KernelPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode caching_mode);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);


	template<typename R>
	struct ShootdownOperation;

	struct [[nodiscard]] ShootdownSender {
		using value_type = void;

		template<typename R>
		friend ShootdownOperation<R>
		connect(ShootdownSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		KernelPageSpace *self;
		VirtualAddr address;
		size_t size;
	};

	ShootdownSender shootdown(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct ShootdownOperation : private ShootNode {
		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		virtual ~ShootdownOperation() = default;

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		bool start_inline() {
			ShootNode::address = s_.address;
			ShootNode::size = s_.size;
			if(s_.self->submitShootdown(this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		ShootdownSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<ShootdownSender>
	operator co_await(ShootdownSender sender) {
		return {sender};
	}

private:
	frg::ticket_spinlock _mutex;
};

struct ClientPageSpace : PageSpace {
	ClientPageSpace();

	ClientPageSpace(const ClientPageSpace &) = delete;

	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access,
			uint32_t flags, CachingMode caching_mode);
	PageStatus unmapSingle4k(VirtualAddr pointer);
	PageStatus cleanSingle4k(VirtualAddr pointer);
	bool isMapped(VirtualAddr pointer);
	bool updatePageAccess(VirtualAddr pointer);

private:
	frg::ticket_spinlock _mutex;
};

} // namespace thor
