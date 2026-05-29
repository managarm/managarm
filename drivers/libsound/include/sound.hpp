#pragma once

#include <async/oneshot-event.hpp>
#include <protocols/mbus/client.hpp>

#include <memory>

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

struct StreamParameters {
	uint32_t sampleRate;
	uint32_t channels;
	uint32_t bufferSize;
	Format format;
};

struct Stream {
	Stream(bool isCapture) : isCapture_{isCapture} { }

	virtual ~Stream() = default;

	virtual frg::expected<Status> setup(const StreamParameters &params) = 0;
	virtual frg::expected<Status> stop() = 0;

	virtual frg::expected<Status> play() = 0;
	virtual frg::expected<Status> pause() = 0;

	virtual frg::expected<Status, size_t> getRemaining() = 0;

	virtual frg::expected<Status, size_t> queueData(const void *data, size_t size) = 0;
	virtual frg::expected<Status> clearData() = 0;

	void onDataChanged(uint64_t change);

	StreamParameters params;
	bool isReady{};
	bool isPaused{};

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
};

async::detached runCard(Card *card);
async::detached runDevice(Device *device);

} // namespace sound
