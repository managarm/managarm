#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>

#include <bit>

#include <arch/dma_pool.hpp>
#include <helix/memory.hpp>

#include <protocols/usb/usb.hpp>

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

	constexpr RawTrb resetEndpoint(uint8_t slotId, uint8_t endpointId) {
		return RawTrb{
			0, 0, 0,
			(uint32_t{slotId} << 24) | (uint32_t{endpointId} << 16)
			| (static_cast<uint32_t>(TrbType::resetEndpointCommand) << 10)
		};
	}

	constexpr RawTrb setTransferRingDequeue(uint8_t slotId, uint8_t endpointId, uintptr_t dequeue) {
		return RawTrb{
			static_cast<uint32_t>(dequeue & 0xFFFFFFFF),
			static_cast<uint32_t>(dequeue >> 32), 0,
			(uint32_t{slotId} << 24) | (uint32_t{endpointId} << 16)
			| (static_cast<uint32_t>(TrbType::setTrDequeuePtrCommand) << 10)
		};
	}
} // namespace Command

namespace Transfer {
	constexpr RawTrb setupStage(protocols::usb::SetupPacket setup, bool hasDataStage, bool dataIn) {
		return RawTrb{
			(uint32_t{setup.value} << 16) | (uint32_t{setup.request} << 8) | uint32_t{setup.type},
			(uint32_t{setup.length} << 16) | uint32_t{setup.index},
			8,
			((hasDataStage ? (dataIn ? 3 : 2) : 0) << 16) | (1 << 6)
				| (static_cast<uint32_t>(TrbType::setupStage) << 10)};
	}

	constexpr RawTrb dataStage(uintptr_t address, size_t size, bool chain, uint32_t tdSize, bool dataIn) {
		if (tdSize > 31)
			tdSize = 31;
		return RawTrb{
			static_cast<uint32_t>(address),
			static_cast<uint32_t>(address >> 32),
			static_cast<uint32_t>(size) | (tdSize << 17),
			(1 << 2) | ((dataIn ? 1 : 0) << 16)
				| ((chain ? 1 : 0) << 4)
				| (static_cast<uint32_t>(TrbType::dataStage) << 10)};
	}

	constexpr RawTrb statusStage(bool dataIn) {
		return RawTrb{
			0, 0, 0,
			((dataIn ? 1 : 0) << 16)
				| (static_cast<uint32_t>(TrbType::statusStage) << 10)};
	}

	constexpr RawTrb normal(uintptr_t address, size_t size, bool chain, uint32_t tdSize) {
		if (tdSize > 31)
			tdSize = 31;
		return RawTrb{
			static_cast<uint32_t>(address),
			static_cast<uint32_t>(address >> 32),
			static_cast<uint32_t>(size) | (tdSize << 17),
			(1 << 2) | ((chain ? 1 : 0) << 4)
				| (static_cast<uint32_t>(TrbType::normal) << 10)};
	}

	constexpr RawTrb withInterrupt(RawTrb trb) {
		trb.val[3] |= 1 << 5;
		return trb;
	}

	template <typename FU, typename FB, typename ...Ts>
	inline void buildTransferChain(size_t maxPacketSize, FU use, arch::dma_buffer_view view, FB build, Ts ...ts) {
		assert(std::popcount(maxPacketSize) == 1);
		size_t tdPacketCount = (view.size() + maxPacketSize - 1) / maxPacketSize;

		size_t progress = 0;
		while(progress < view.size()) {
			uintptr_t ptr = (uintptr_t)view.data() + progress;
			uintptr_t pptr = helix::addressToPhysical(ptr);

			auto chunk = std::min(view.size() - progress, 0x1000 - (ptr & 0xFFF));

			bool chain = (progress + chunk) < view.size();

			// TODO(qookie): For xHCI 0.96 and older, this should be (progress + chunk) >> 10.
			auto tdSize = tdPacketCount - (progress + chunk) / maxPacketSize;
			// Last TRB always has TD Size = 0
			if ((progress + chunk) == view.size())
				tdSize = 0;

			auto trb = build(pptr, chunk, chain, tdSize, ts...);
			if (!chain)
				trb = withInterrupt(trb);

			use(trb);
			progress += chunk;
		}
	}

	template <typename FU>
	inline void buildNormalChain(FU use, arch::dma_buffer_view view, size_t maxPacketSize) {
		buildTransferChain(maxPacketSize, use, view, normal);
	}

	template <typename FU>
	inline void buildControlChain(FU use, protocols::usb::SetupPacket setup, arch::dma_buffer_view view, bool dataIn, size_t maxPacketSize) {
		bool statusIn = true;

		if (view.size() && dataIn)
			statusIn = false;

		use(withInterrupt(setupStage(setup, view.size(), dataIn)));
		buildTransferChain(maxPacketSize, use, view, dataStage, dataIn);
		use(withInterrupt(statusStage(statusIn)));
	}
} // namespace Transfer
