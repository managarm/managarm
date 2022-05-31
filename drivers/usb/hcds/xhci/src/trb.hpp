#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>

struct RawTrb {
	uint32_t val[4];
};
static_assert(sizeof(RawTrb) == 16, "invalid trb size");

enum class TrbType : uint8_t {
	reserved = 0,

	// Transfer ring TRBs
	normal,
	setupStage,
	dataStage,
	statusStage,
	isoch,
	link, // Also applies to the command ring
	eventData,
	noop,

	// Command ring TRBs
	enableSlotCommand,
	disableSlotCommand,
	addressDeviceCommand,
	configureEndpointCommand,
	evalContextCommand,
	resetEndpointCommand,
	stopEndpointCommand,
	setTrDequeuePtrCommand,
	resetDeviceCommand,
	forceEventCommand,
	negotiateBandwidthCommand,
	setLatencyToleranceValCommand,
	getPortBandwidthCommand,
	forceHeaderCommand,
	noopCommand,
	getExtPropertyCommand,
	setExtPropertyCommand,

	// Event ring TRBs
	transferEvent = 32,
	commandCompletionEvent,
	portStatusChangeEvent,
	bandwidthRequestEvent,
	doorbellEvent,
	hostControllerEvent,
	deviceNotificationEvent,
	mfindexWrapEvent
};

namespace Command {
	constexpr RawTrb enableSlot(uint8_t slotType) {
		return RawTrb{
			0, 0, 0, 
			(uint32_t{slotType} << 16) | (static_cast<uint32_t>(
					TrbType::enableSlotCommand) << 10)
		};
	}

	constexpr RawTrb addressDevice(uint8_t slotId, uintptr_t inputCtx) {
		assert(!(inputCtx & 0xF));

		return RawTrb{
			static_cast<uint32_t>(inputCtx & 0xFFFFFFFF),
			static_cast<uint32_t>(inputCtx >> 32), 0,
			(uint32_t{slotId} << 24) | (static_cast<uint32_t>(
					TrbType::addressDeviceCommand) << 10)
		};
	}

	constexpr RawTrb configureEndpoint(uint8_t slotId, uintptr_t inputCtx) {
		assert(!(inputCtx & 0xF));

		return RawTrb{
			static_cast<uint32_t>(inputCtx & 0xFFFFFFFF),
			static_cast<uint32_t>(inputCtx >> 32), 0,
			(uint32_t{slotId} << 24) | (static_cast<uint32_t>(
					TrbType::configureEndpointCommand) << 10)
		};
	}

	constexpr RawTrb evaluateContext(uint8_t slotId, uintptr_t inputCtx) {
		assert(!(inputCtx & 0xF));

		return RawTrb{
			static_cast<uint32_t>(inputCtx & 0xFFFFFFFF),
			static_cast<uint32_t>(inputCtx >> 32), 0,
			(uint32_t{slotId} << 24) | (static_cast<uint32_t>(
					TrbType::evalContextCommand) << 10)
		};
	}
} // namespace Command
