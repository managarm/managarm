#pragma once

#include <thor-internal/arch/cpu-data.hpp>

namespace thor {

// Note: These constants we mirrored in assembly.
// Do not change their values!
inline constexpr unsigned int uarRead = 1;
inline constexpr unsigned int uarWrite = 2;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct UserAccessRegion {
	void *startIp;
	void *endIp;
	void *faultIp;
	unsigned int flags;
};

struct IseqRegion;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct IseqContext {
	// Bits for state.
	static constexpr uint8_t STATE_TX = 1; // Currently inside transaction.
	static constexpr uint8_t STATE_INTERRUPTED = 2; // Transaction was interrupted.

	IseqRegion *region{nullptr};
	// state can be changed by interrupt contexts. Access through std::atomic_ref.
	uint8_t state{0};
};

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct IseqRegion {
	void *startIp;
	void *commitIp;
	// IP that is restored on interrupt.
	// Must be outside [startIp, commitIp).
	void *interruptIp;
};

// RAII class to enclose a transaction on an IseqContext.
// All iseq* functions below must be called within an IseqTransaction.
struct IseqTransaction {
	IseqTransaction()
	: _ctx{getPlatformCpuData()->iseqPtr} {
		std::atomic_ref state_ref{_ctx->state};

		auto state = state_ref.load(std::memory_order_relaxed);
		assert(!(state & IseqContext::STATE_TX));
		assert(!(state & IseqContext::STATE_INTERRUPTED));
		state_ref.store(IseqContext::STATE_TX, std::memory_order_relaxed);
	}

	IseqTransaction(const IseqTransaction &) = delete;

	~IseqTransaction() {
		assert(_ctx == getPlatformCpuData()->iseqPtr);
		std::atomic_ref state_ref{_ctx->state};

		auto state = state_ref.load(std::memory_order_relaxed);
		assert(state & IseqContext::STATE_TX);
		state_ref.store(0, std::memory_order_relaxed);
	}

	IseqTransaction &operator= (const IseqTransaction &) = delete;

private:
	IseqContext *_ctx;
};

// The following iseq* functions return true on success and false if the transaction was interrupted.
//
// Note that the transactional semantics differ among these function.
//
// * iseqStore64() is transactional; i.e., it performs a write only if it succeeds.
//
// * iseqCopyWeak() has weaker transactional semantics.
//   In particular, some bytes may have already been written even in the failure case.
//   However, in contrast to a plain memcpy(), it is still guaranteed that no bytes
//   are written after the transaction is interrupted.

extern "C" bool iseqStore64(uint64_t *p, uint64_t v);
extern "C" bool iseqCopyWeak(void *dest, const void *src, size_t size);

} // namespace thor
