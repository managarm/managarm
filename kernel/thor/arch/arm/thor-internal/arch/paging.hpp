#pragma once

#include <atomic>

#include <assert.h>
#include <frg/list.hpp>
#include <smarter.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>

#include <thor-internal/arch-generic/asid.hpp>

namespace thor {

enum {
	kPageSize = 0x1000,
	kPageShift = 12
};

constexpr Word kPfAccess = 1;
constexpr Word kPfWrite = 2;
constexpr Word kPfUser = 4;
constexpr Word kPfBadTable = 8;
constexpr Word kPfInstruction = 16;

inline void *mapDirectPhysical(PhysicalAddr physical) {
	assert(physical < 0x4000'0000'0000);
	return reinterpret_cast<void *>(0xFFFF'8000'0000'0000 + physical);
}

inline PhysicalAddr reverseDirectPhysical(void *pointer) {
	return reinterpret_cast<uintptr_t>(pointer) - 0xFFFF'8000'0000'0000;
}

void invalidatePage(const void *address);
void invalidateAsid(int asid);
void invalidatePage(int pcid, const void *address);

struct PageAccessor {
	friend void swap(PageAccessor &a, PageAccessor &b) {
		using std::swap;
		swap(a._pointer, b._pointer);
	}

	PageAccessor()
	: _pointer{nullptr} { }

	PageAccessor(PhysicalAddr physical) {
		assert(physical != PhysicalAddr(-1) && "trying to access invalid physical page");
		assert(!(physical & (kPageSize - 1)) && "physical page is not aligned");
		assert(physical < 0x4000'0000'0000);
		_pointer = reinterpret_cast<void *>(0xFFFF'8000'0000'0000 + physical);
	}

	PageAccessor(const PageAccessor &) = delete;

	PageAccessor(PageAccessor &&other)
	: PageAccessor{} {
		swap(*this, other);
	}

	~PageAccessor() { }

	PageAccessor &operator= (PageAccessor other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _pointer;
	}

	void *get() {
		return _pointer;
	}

private:
	void *_pointer;
};

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);

namespace page_mode {
	static constexpr uint32_t remap = 1;
}

enum class PageMode {
	null,
	normal,
	remap
};

using PageFlags = uint32_t;

namespace page_access {
	static constexpr uint32_t write = 1;
	static constexpr uint32_t execute = 2;
	static constexpr uint32_t read = 4;
}

using PageStatus = uint32_t;

namespace page_status {
	static constexpr PageStatus present = 1;
	static constexpr PageStatus dirty = 2;
};

enum class CachingMode {
	null,
	uncached,
	writeCombine,
	writeThrough,
	writeBack,
	mmio,
	mmioNonPosted
};

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
public:
	struct Walk {
		Walk(ClientPageSpace *space);

		Walk(const Walk &) = delete;

		~Walk();

		Walk &operator= (const Walk &) = delete;

		void walkTo(uintptr_t address);

		PageFlags peekFlags();
		PhysicalAddr peekPhysical();

	private:
		ClientPageSpace *_space;

		void _update();

		uintptr_t _address = 0;

		// Accessors for all levels of PTs.
		PageAccessor _accessor4; // Coarsest level (PML4).
		PageAccessor _accessor3;
		PageAccessor _accessor2;
		PageAccessor _accessor1; // Finest level (page table).
	};

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
