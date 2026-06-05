#pragma once

#include <async/oneshot-event.hpp>
#include <protocols/mbus/client.hpp>

#include <memory>
#include <functional>
#include <algorithm>
#include <span>

namespace sound {

enum class Status {
	success,
	invalidParameters
};

enum class Format {
	pcmS8,
	pcmS16,
	pcmS20,
	pcmS24,
	pcmS32
};

struct PeriodChunk {
	void *virt;
	uintptr_t phys;
};

struct StreamParameters {
	uint32_t sampleRate;
	uint32_t channels;
	uint32_t periodCount;
	uint32_t periodSize;
	Format format;
	std::function<void()> periodCallback;
	std::vector<PeriodChunk> periodChunks;
};

struct Stream {
	Stream(bool isCapture) : isCapture_{isCapture} { }

	virtual ~Stream() = default;

	virtual frg::expected<Status> setup(const StreamParameters &newParams) = 0;
	virtual frg::expected<Status> stop() = 0;

	virtual frg::expected<Status> play() = 0;
	virtual frg::expected<Status> pause() = 0;

	virtual frg::expected<Status, size_t> getPosition() = 0;

	StreamParameters params{};
	bool isReady{};
	bool isPaused{true};

	constexpr bool isCapture() const {
		return isCapture_;
	}

private:
	bool isCapture_;
};

struct Device;

struct Card {
	std::vector<std::unique_ptr<Stream>> playbackStreams;
	std::vector<std::unique_ptr<Stream>> captureStreams;
	std::vector<std::unique_ptr<Device>> devices;

	Stream *findFreePlaybackStream() const {
		for (auto &stream : playbackStreams) {
			if (!stream->isReady) {
				return stream.get();
			}
		}

		return nullptr;
	}

	Stream *findFreeCaptureStream() const {
		for (auto &stream : captureStreams) {
			if (!stream->isReady) {
				return stream.get();
			}
		}

		return nullptr;
	}

	async::oneshot_event mbusPublishedEvent;
	mbus_ng::EntityId mbusId{};
};

enum class DeviceType {
	playback,
	capture
};

struct DeviceLimits {
	struct Range {
		uint32_t min;
		uint32_t max;
	};

	std::vector<uint32_t> sampleRates;
	std::vector<Format> formats;
	Range channels;
	Range periodCount;
	Range periodSize;
	uint32_t periodSizeAlign;
	bool forcePow2PeriodSizes;
};

struct Device {
	Device(DeviceType type, mbus_ng::EntityId parentId, Card *card) : type{type}, parentId{parentId}, card{card} { }

	virtual ~Device() = default;

	virtual frg::expected<Status> attachToStream(Stream *stream) = 0;

	virtual frg::expected<Status> mute() = 0;
	virtual frg::expected<Status> unmute() = 0;
	virtual frg::expected<Status> setVolume(uint32_t volume) = 0;

	DeviceType type;
	mbus_ng::EntityId parentId;

	Card *card;
	Stream *attachedStream{};

	DeviceLimits limits{};
};

async::detached runCard(Card *card, uint64_t numDevices);
async::detached runDevice(Device *device);

} // namespace sound
