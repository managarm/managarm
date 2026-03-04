#pragma once

#include <frg/optional.hpp>
#include <frg/rcu_radixtree.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

struct CachePage;

struct PfnDescriptor {
	static PfnDescriptor cachePage(CachePage *ptr) {
		auto ptrBits = reinterpret_cast<uintptr_t>(ptr);
		assert(!(ptrBits & typeMask));
		return PfnDescriptor{ptrBits | typeCache};
	}

	static PfnDescriptor otherPage() {
		return PfnDescriptor{typeOther};
	}

	static PfnDescriptor hardwarePage(uint64_t refCount) {
		return PfnDescriptor{(refCount << typeShift) | typeHardware};
	}

	PfnDescriptor() = default;

	explicit PfnDescriptor(uint64_t bits)
	: bits_{bits} { }

	uint64_t bits() {
		return bits_;
	}

	explicit operator bool() {
		return bits_ != 0;
	}

	bool isCachePage() const {
		return (bits_ & typeMask) == typeCache;
	}

	bool isHardware() const {
		return (bits_ & typeMask) == typeHardware;
	}

	CachePage *cachePagePtr() const {
		assert((bits_ & typeMask) == typeCache);
		return reinterpret_cast<CachePage *>(bits_ & ~typeMask);
	}

	uint64_t hardwareRefCount() const {
		assert((bits_ & typeMask) == typeHardware);
		return bits_ >> typeShift;
	}

private:
	static constexpr int typeShift = 3;
	static constexpr uint64_t typeMask = (UINT64_C(1) << typeShift) - 1;

	static constexpr uint64_t typeNull = 0;
	static constexpr uint64_t typeCache = 1;
	static constexpr uint64_t typeOther = 2;
	static constexpr uint64_t typeHardware = 3;

	uint64_t bits_{0};
};

static_assert(std::atomic<PfnDescriptor>::is_always_lock_free);

struct PfnDb {
	PfnDb() = default;

	PfnDb(const PfnDb &) = delete;

	PfnDb &operator= (const PfnDb &) = delete;

	// Lock-free but protected by RCU.
	frg::optional<PfnDescriptor> find(uint64_t pa) {
		assert(!(pa & (kPageSize - 1)));
		IplGuard<ipl::schedule> guard;

		auto it = tree_.find(pa);
		if(!it)
			return frg::null_opt;
		return it->load(std::memory_order_relaxed);
	}

	void insert(uint64_t pa, PfnDescriptor descriptor) {
		auto lock = frg::guard(&mutex_);

		auto [it, wasInserted] = tree_.find_or_insert(pa);
		if (!wasInserted)
			panicLogger() << "thor: PFN collision for address 0x"
				<< frg::hex_fmt{pa}
				<< ", existing entry is 0x"
				<< frg::hex_fmt{it->load(std::memory_order_relaxed).bits()}
				<< frg::endlog;
		it->store(descriptor, std::memory_order_relaxed);
	}

	void erase(uint64_t pa) {
		auto lock = frg::guard(&mutex_);

		tree_.erase(pa);
	}

	// Calls fn with the current value (or frg::null_opt on insertion),
	// then replaces the descriptor by the return value of fn.
	template<typename F>
	requires requires(F fn, frg::optional<PfnDescriptor> descriptor) {
		{ fn(descriptor) } -> std::same_as<PfnDescriptor>;
	}
	void insertOrExchange(uint64_t pa, F &&fn) {
		auto lock = frg::guard(&mutex_);

		auto [it, wasInserted] = tree_.find_or_insert(pa);
		frg::optional<PfnDescriptor> current;
		if (!wasInserted)
			current = it->load(std::memory_order_relaxed);
		it->store(fn(current), std::memory_order_relaxed);
	}

	// Calls fn with the current value, then either replaces the descriptor
	// by the return value of fn or erases the descriptor in the case of frg::null_opt.
	template<typename F>
	requires requires(F fn, PfnDescriptor descriptor) {
		{ fn(descriptor) } -> std::same_as<frg::optional<PfnDescriptor>>;
	}
	void exchangeOrErase(uint64_t pa, F &&fn) {
		auto lock = frg::guard(&mutex_);

		auto it = tree_.find(pa);
		assert(it);
		auto result = fn(it->load(std::memory_order_relaxed));
		if(result) {
			it->store(*result, std::memory_order_relaxed);
		} else {
			tree_.erase(pa);
		}
	}

private:
	IrqSpinlock mutex_;
	frg::rcu_radixtree<std::atomic<PfnDescriptor>, Allocator, RcuPolicy> tree_;
};

PfnDb &globalPfnDb();

} // namespace thor
