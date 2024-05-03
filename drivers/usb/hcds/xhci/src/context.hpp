#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>

#include <arch/dma_pool.hpp>

struct RawContext {
	uint32_t val[8];
};

inline constexpr size_t inputCtxCtrl = 0;
inline constexpr size_t inputCtxSlot = 1;
inline constexpr size_t inputCtxEp0 = 2;

inline constexpr size_t deviceCtxSlot = 0;
inline constexpr size_t deviceCtxEp0 = 1;

template <size_t Size>
struct ContextArray {
	ContextArray() = default;

	ContextArray(bool largeCtx, arch::os::contiguous_pool *pool)
	: largeCtx_{largeCtx} {
		if (largeCtx) {
			largeArr_ = arch::dma_object<LargeArr>{pool};
			ctx_ = &largeArr_->ctx[0];
		} else {
			smallArr_ = arch::dma_object<SmallArr>{pool};
			ctx_ = &smallArr_->ctx[0];
		}

		memset(ctx_, 0, rawSize());
	}

	void *rawData() {
		return ctx_;
	}

	size_t rawSize() {
		return largeCtx_ ? sizeof(LargeArr) : sizeof(SmallArr);
	}

	RawContext &get(size_t i) {
		return ctx_[i * (largeCtx_ ? 2 : 1)];
	}

private:
	struct alignas(64) SmallArr {
		RawContext ctx[Size];
	};

	struct alignas(64) LargeArr {
		RawContext ctx[Size * 2];
	};

	bool largeCtx_ = false;
	arch::dma_object<SmallArr> smallArr_{};
	arch::dma_object<LargeArr> largeArr_{};
	RawContext *ctx_ = nullptr;
};

using InputContext = ContextArray<34>;
using DeviceContext = ContextArray<32>;

struct ContextField {
	int word;
	uint32_t value;
};

constexpr RawContext &operator|=(RawContext &ctx, ContextField field) {
	ctx.val[field.word] |= field.value;
	return ctx;
}

constexpr RawContext &operator&=(RawContext &ctx, ContextField field) {
	ctx.val[field.word] &= field.value;
	return ctx;
}

constexpr ContextField operator~(ContextField field) {
	field.value = ~field.value;
	return field;
}

namespace InputControlFields {
	constexpr ContextField drop(int v) {
		return {0, 1u << v};
	}

	constexpr ContextField add(int v) {
		return {1, 1u << v};
	}

	constexpr ContextField config(uint8_t v) {
		return {7, uint32_t{v}};
	}

	constexpr ContextField interface(uint8_t v) {
		return {7, uint32_t{v} << 8};
	}

	constexpr ContextField alternate(uint8_t v) {
		return {7, uint32_t{v} << 16};
	}
} // namespace InputControlFields

namespace SlotFields {
	constexpr ContextField routeString(uint32_t v) {
		return {0, v & 0xFFFFFu};
	}

	constexpr ContextField speed(uint8_t v) {
		return {0, uint32_t{v & 0xFu} << 20};
	}

	constexpr ContextField mtt(bool v) {
		return {0, uint32_t{v} << 25};
	}

	constexpr ContextField hub(bool v) {
		return {0, uint32_t{v} << 26};
	}

	constexpr ContextField ctxEntries(uint8_t v) {
		return {0, uint32_t{v & 0x1Fu} << 27};
	}

	constexpr ContextField maxExitLatency(uint16_t v) {
		return {1, v};
	}

	constexpr ContextField rootHubPort(uint8_t v) {
		return {1, uint32_t{v} << 16};
	}

	constexpr ContextField portCount(uint8_t v) {
		return {1, uint32_t{v} << 24};
	}

	constexpr ContextField parentHubSlot(uint8_t v) {
		return {2, uint32_t{v}};
	}

	constexpr ContextField parentHubPort(uint8_t v) {
		return {2, uint32_t{v} << 8};
	}

	constexpr ContextField ttThinkTime(uint8_t v) {
		return {1, uint32_t{v & 0b11u} << 16};
	}

	constexpr ContextField interrupterTarget(uint16_t v) {
		return {1, uint32_t{v & 0xFFFFFu} << 22};
	}
} // namespace SlotFields

namespace EpFields {
	constexpr ContextField interval(uint8_t v) {
		return {0, uint32_t{v} << 16};
	}

	constexpr ContextField maxEsitPayloadHi(uint32_t v) {
		return {0, uint32_t{(v >> 16) & 0xFF} << 24};
	}

	constexpr ContextField errorCount(uint8_t v) {
		return {1, uint32_t{v & 0b11u} << 1};
	}

	constexpr ContextField epType(uint8_t v) {
		return {1, uint32_t{v & 0b111u} << 3};
	}

	constexpr ContextField maxPacketSize(uint16_t v) {
		return {1, uint32_t{v} << 16};
	}

	constexpr ContextField dequeCycle(bool v) {
		return {2, uint32_t{v}};
	}

	constexpr ContextField trPointerLo(uintptr_t v) {
		assert(!(v & 0xF));
		return {2, static_cast<uint32_t>(v & 0xFFFFFFF0u)};
	}

	constexpr ContextField trPointerHi(uintptr_t v) {
		assert(!(v & 0xF));
		return {3, static_cast<uint32_t>(v >> 32)};
	}

	constexpr ContextField averageTrbLength(uint16_t v) {
		return {4, uint32_t{v}};
	}

	constexpr ContextField maxEsitPayloadLo(uint32_t v) {
		return {4, uint32_t{v & 0xFFFF} << 16};
	}
} // namespace EpFields
