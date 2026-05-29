#pragma once

#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include <sound.hpp>

struct UhdaController;
struct UhdaCodec;
struct UhdaStream;
struct UhdaPath;

struct Controller final : sound::Card {
	Controller(protocols::hw::Device device, bool msiAvailable) : device{std::move(device)}, uhda{}, codecs{},
			codecCount{}, msiAvailable{msiAvailable}, useMsi{} { }

	async::result<void> run();

	protocols::hw::Device device;
	UhdaController *uhda;
	const UhdaCodec *const *codecs;
	size_t codecCount;
	bool msiAvailable;
	bool useMsi;
};

struct Stream final : sound::Stream {
	Stream(UhdaStream *uhda, bool isCapture) : sound::Stream{isCapture}, uhda{uhda} { }

	frg::expected<sound::Status> setup(const sound::StreamParameters &params) override;
	frg::expected<sound::Status> stop() override;

	frg::expected<sound::Status> play() override;
	frg::expected<sound::Status> pause() override;

	frg::expected<sound::Status, size_t> getRemaining() override;

	frg::expected<sound::Status, size_t> queueData(const void *data, size_t size) override;
	frg::expected<sound::Status> clearData() override;

	UhdaStream *uhda;
};

struct Device final : sound::Device {
	Device(Controller *ctrl, UhdaPath *path, sound::DeviceType type, mbus_ng::EntityId parentId)
			: sound::Device{type, parentId, ctrl}, path{path} { }

	frg::expected<sound::Status> attachToStream(sound::Stream *stream) override;

	frg::expected<sound::Status> mute() override;
	frg::expected<sound::Status> unmute() override;
	frg::expected<sound::Status> setVolume(uint32_t volume) override;

	UhdaPath *path;
};
