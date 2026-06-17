#include "controller.hpp"

#include <uhda/uhda.h>

#include <print>

std::vector<arch::dma_buffer_view> globalPeriodChunks;

Device::Device(Controller *ctrl, UhdaPath *path, sound::DeviceType type, mbus_ng::EntityId parentId)
		: sound::Device{type, parentId, ctrl}, path{path} {
	auto info = uhda_path_get_info(path);

	for (uint32_t i = 0; i < info.supported_sample_rate_count; i++) {
		limits.sampleRates.push_back(info.supported_sample_rates[i]);
	}

	for (uint32_t i = 0; i < info.supported_formats_count; i++) {
		switch (info.supported_formats[i]) {
		case UHDA_FORMAT_PCM8:
			limits.formats.push_back(sound::Format::pcmS8);
			break;
		case UHDA_FORMAT_PCM16:
			limits.formats.push_back(sound::Format::pcmS16);
			break;
		case UHDA_FORMAT_PCM20:
			limits.formats.push_back(sound::Format::pcmS20);
			break;
		case UHDA_FORMAT_PCM24:
			limits.formats.push_back(sound::Format::pcmS24);
			break;
		case UHDA_FORMAT_PCM32:
			limits.formats.push_back(sound::Format::pcmS32);
			break;
		}
	}

	limits.channels.min = 1;
	limits.channels.max = 16;

	limits.periodCount.min = UHDA_MIN_PERIODS;
	limits.periodCount.max = UHDA_MAX_PERIODS;

	limits.periodSize.min = 128;
	limits.periodSize.max = 0x1000;

	limits.periodSizeAlign = 128;
	limits.forcePow2PeriodSizes = true;
}

async::result<void> Controller::run(uint64_t numDevices) {
	sound::runCard(this, numDevices);
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
		.fmt = fmt,
		.period_count = params.periodCount,
		.period_size = params.periodSize,
		.period_callback_distance = 1,
		.period_callback = [](UhdaStream *, void *arg) {
			(*static_cast<std::function<void()> *>(arg))();
		},
		.period_callback_arg = (void *) &params.periodCallback
	};
}

frg::expected<sound::Status> Stream::setup(const sound::StreamParameters &newParams) {
	params = newParams;
	auto uhdaParams = paramsToUhda(params);

	globalPeriodChunks = std::move(params.periodChunks);

	auto status = uhda_stream_setup(uhda, &uhdaParams);
	assert(status == UHDA_STATUS_SUCCESS);

	isReady = true;

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

frg::expected<sound::Status, size_t> Stream::getPosition() {
	uint32_t position = uhda_stream_get_position(uhda);
	return static_cast<size_t>(position);
}

frg::expected<sound::Status> Device::attachToStream(sound::Stream *stream) {
	auto *hdaStream = static_cast<Stream *>(stream);

	auto params = paramsToUhda(stream->params);

	auto status = uhda_path_setup(path, &params, hdaStream->uhda);
	if (status != UHDA_STATUS_SUCCESS) {
		std::println(std::cout, "sound/hda: uhda_path_setup failed: {}", static_cast<int>(status));
	}
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
