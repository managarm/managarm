#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>

struct RawContext {
	uint32_t val[8];
};

struct alignas(64) InputContext {
	RawContext inputControlContext;
	RawContext slotContext;
	RawContext endpointContext[31];
};
static_assert (sizeof(InputContext) == 34 * 32, "invalid InputContext size"); // 34 due to 64 byte alignment

struct alignas(64) DeviceContext {
	RawContext slotContext;
	RawContext endpointContext[31];
};
static_assert (sizeof(DeviceContext) == 32 * 32, "invalid DeviceContext size");

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
} // namespace EpFields
