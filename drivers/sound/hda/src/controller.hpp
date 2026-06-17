#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include <sound.hpp>

struct UhdaController;
struct UhdaCodec;
struct UhdaStream;
struct UhdaPath;

struct Controller final : sound::Card {
	Controller(
	    protocols::hw::Device device,
	    bool msiAvailable,
	    helix::UniqueDescriptor dmaSpaceHandle,
	    bool iommuActive
	)
	: Card(&pool_),
	  device{std::move(device)},
	  uhda{},
	  codecs{},
	  codecCount{},
	  msiAvailable{msiAvailable},
	  useMsi{},
	  dmaSpaceHandle_{std::move(dmaSpaceHandle)},
	  dmaSpace{pool_.attachDmaSpace(dmaSpaceHandle_, iommuActive)} {}

	async::result<void> run(uint64_t numDevices);

	protocols::hw::Device device;
	UhdaController *uhda;
	const UhdaCodec *const *codecs;
	size_t codecCount;
	bool msiAvailable;
	bool useMsi;

private:
	arch::contiguous_pool pool_{{.addressBits = 64, .allocateContigous = true}};
	helix::UniqueDescriptor dmaSpaceHandle_;

public:
	arch::dma_space dmaSpace;
	std::unordered_map<uintptr_t, arch::dma_buffer> physicalMappings;
};

struct Stream final : sound::Stream {
	Stream(UhdaStream *uhda, bool isCapture) : sound::Stream{isCapture}, uhda{uhda} { }

	frg::expected<sound::Status> setup(const sound::StreamParameters &params) override;
	frg::expected<sound::Status> stop() override;

	frg::expected<sound::Status> play() override;
	frg::expected<sound::Status> pause() override;

	frg::expected<sound::Status, size_t> getPosition() override;

	UhdaStream *uhda;
};

struct Device final : sound::Device {
	Device(Controller *ctrl, UhdaPath *path, sound::DeviceType type, mbus_ng::EntityId parentId);

	frg::expected<sound::Status> attachToStream(sound::Stream *stream) override;

	frg::expected<sound::Status> mute() override;
	frg::expected<sound::Status> unmute() override;
	frg::expected<sound::Status> setVolume(uint32_t volume) override;

	UhdaPath *path;
};
