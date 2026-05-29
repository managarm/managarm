#include "controller.hpp"

#include <uhda/uhda.h>

#include <print>

async::result<void> Controller::run() {
	sound::runCard(this);
	co_return;
}

static UhdaStreamParams paramsToUhda(const sound::StreamParameters &params) {
	UhdaFormat fmt{};
	switch (params.format) {
	case sound::Format::pcmS8:
		fmt = UHDA_FORMAT_PCM8;
		break;	
	case sound::Format::pcmS16:
		fmt = UHDA_FORMAT_PCM16;
		break;
	case sound::Format::pcmS20:
		fmt = UHDA_FORMAT_PCM20;
		break;
	case sound::Format::pcmS24:
		fmt = UHDA_FORMAT_PCM24;
		break;
	case sound::Format::pcmS32:
		fmt = UHDA_FORMAT_PCM32;
		break;
	}

	return {
		.sample_rate = params.sampleRate,
		.channels = params.channels,
		.fmt = fmt
	};
}

frg::expected<sound::Status> Stream::setup(const sound::StreamParameters &params) {
	auto uhdaParams = paramsToUhda(params);

	auto status = uhda_stream_setup(uhda, &uhdaParams, params.bufferSize, nullptr, nullptr,
			0, nullptr, nullptr);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Stream::stop() {
	auto status = uhda_stream_play(uhda, false);
	assert(status == UHDA_STATUS_SUCCESS);
	status = uhda_stream_shutdown(uhda);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Stream::play() {
	auto status = uhda_stream_play(uhda, true);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Stream::pause() {
	auto status = uhda_stream_play(uhda, false);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status, size_t> Stream::getRemaining() {
	uint32_t remaining;
	auto status = uhda_stream_get_remaining(uhda, &remaining);
	assert(status == UHDA_STATUS_SUCCESS);
	return static_cast<size_t>(remaining);
}

frg::expected<sound::Status, size_t> Stream::queueData(const void *data, size_t size) {
	uint32_t uhdaSize = size;
	auto status = uhda_stream_queue_data(uhda, data, &uhdaSize);
	assert(status == UHDA_STATUS_SUCCESS);
	return static_cast<size_t>(uhdaSize);
}

frg::expected<sound::Status> Stream::clearData() {
	auto status = uhda_stream_clear_queue(uhda);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Device::attachToStream(sound::Stream *stream) {
	auto *hdaStream = static_cast<Stream *>(stream);

	auto params = paramsToUhda(stream->params);

	auto status = uhda_path_setup(path, &params, hdaStream->uhda);
	assert(status == UHDA_STATUS_SUCCESS);

	attachedStream = stream;
	return frg::success;
}

frg::expected<sound::Status> Device::mute() {
	auto status = uhda_path_mute(path, true);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Device::unmute() {
	auto status = uhda_path_mute(path, false);
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}

frg::expected<sound::Status> Device::setVolume(uint32_t volume) {
	auto status = uhda_path_set_volume(path, static_cast<int>(volume));
	assert(status == UHDA_STATUS_SUCCESS);
	return frg::success;
}
