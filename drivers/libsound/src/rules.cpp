#include "rules.hpp"

namespace {
constexpr uint32_t usInSecond = 1000 * 1000;
}

namespace alsa::rules {

Rule sampleBitsRule{
	SNDRV_PCM_HW_PARAM_SAMPLE_BITS,
	{SNDRV_PCM_HW_PARAM_FORMAT},
	[](Params &params) {
		auto &formats = params.mask(SNDRV_PCM_HW_PARAM_FORMAT);

		auto has_format = [&](uint32_t format) {
			return formats.bits[0] & (1U << format);
		};

		snd_interval constraint{};
		constraint.min = UINT32_MAX;
		constraint.integer = 1;

		if (has_format(SNDRV_PCM_FORMAT_S8)) {
			constraint.min = 8;
			constraint.max = 8;
		}
		if (has_format(SNDRV_PCM_FORMAT_S16_LE)) {
			constraint.min = std::min(constraint.min, 16U);
			constraint.max = std::max(constraint.max, 16U);
		}
		if (has_format(SNDRV_PCM_FORMAT_S20_LE)) {
			constraint.min = std::min(constraint.min, 20U);
			constraint.max = std::max(constraint.max, 20U);
		}
		if (has_format(SNDRV_PCM_FORMAT_S24_LE)) {
			constraint.min = std::min(constraint.min, 24U);
			constraint.max = std::max(constraint.max, 24U);
		}
		if (has_format(SNDRV_PCM_FORMAT_S32_LE)) {
			constraint.min = std::min(constraint.min, 32U);
			constraint.max = std::max(constraint.max, 32U);
		}

		if (constraint.min == UINT32_MAX)
			return;

		params.intersect_interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS, constraint);
	}
};

Rule sampleBitsFromFrameBitsRule{
	SNDRV_PCM_HW_PARAM_SAMPLE_BITS,
	{SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_CHANNELS},
	[](Params &params) {
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);
		auto &channels = params.interval(SNDRV_PCM_HW_PARAM_CHANNELS);

		auto constraint = utils::intervalDiv(frameBits, channels);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS, constraint);
	}
};

Rule frameBitsRule{
	SNDRV_PCM_HW_PARAM_FRAME_BITS,
	{SNDRV_PCM_HW_PARAM_SAMPLE_BITS, SNDRV_PCM_HW_PARAM_CHANNELS},
	[](Params &params) {
		auto &sampleBits = params.interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
		auto &channels = params.interval(SNDRV_PCM_HW_PARAM_CHANNELS);

		auto constraint = utils::intervalMul(sampleBits, channels);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_FRAME_BITS, constraint);
	}
};

Rule frameBitsFromPeriodBytesRule{
	SNDRV_PCM_HW_PARAM_FRAME_BITS,
	{SNDRV_PCM_HW_PARAM_PERIOD_BYTES, SNDRV_PCM_HW_PARAM_PERIOD_SIZE},
	[](Params &params) {
		auto &periodBytes = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);

		auto constraint = utils::intervalMulDiv(periodBytes, 8, periodSize);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_FRAME_BITS, constraint);
	}
};

Rule frameBitsFromBufferBytesRule{
	SNDRV_PCM_HW_PARAM_FRAME_BITS,
	{SNDRV_PCM_HW_PARAM_BUFFER_BYTES, SNDRV_PCM_HW_PARAM_BUFFER_SIZE},
	[](Params &params) {
		auto &bufferBytes = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
		auto &bufferSizeFrames = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

		auto constraint = utils::intervalMulDiv(bufferBytes, 8, bufferSizeFrames);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_FRAME_BITS, constraint);
	}
};

Rule channelsRule{
	SNDRV_PCM_HW_PARAM_CHANNELS,
	{SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_SAMPLE_BITS},
	[](Params &params) {
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);
		auto &sampleBits = params.interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS);

		auto constraint = utils::intervalDiv(frameBits, sampleBits);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_CHANNELS, constraint);
	}
};

Rule rateRule{
	SNDRV_PCM_HW_PARAM_RATE,
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_TIME},
	[](Params &params) {
		auto &periodSizeFrames = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		auto &periodTime = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_TIME);

		auto constraint = utils::intervalMulDiv(periodSizeFrames, usInSecond, periodTime);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_RATE, constraint);
	}
};

Rule rateFromBufferSizeRule{
	SNDRV_PCM_HW_PARAM_RATE,
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_BUFFER_TIME},
	[](Params &params) {
		auto &bufferSizeFrames = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &bufferTime = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_TIME);

		auto constraint = utils::intervalMulDiv(bufferSizeFrames, usInSecond, bufferTime);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_RATE, constraint);
	}
};

Rule periodsRule{
	SNDRV_PCM_HW_PARAM_PERIODS,
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_SIZE},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);

		auto constraint = utils::intervalDiv(bufferSize, periodSize);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIODS, constraint);
	}
};

Rule periodSizeRule{
	SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIODS},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &periods = params.interval(SNDRV_PCM_HW_PARAM_PERIODS);

		auto constraint = utils::intervalDiv(bufferSize, periods);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, constraint);
	}
};

Rule periodSizeFromPeriodBytesRule{
	SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
	{SNDRV_PCM_HW_PARAM_PERIOD_BYTES, SNDRV_PCM_HW_PARAM_FRAME_BITS},
	[](Params &params) {
		auto &periodBytes = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);

		auto constraint = utils::intervalMulDiv(periodBytes, 8, frameBits);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, constraint);
	}
};

Rule periodSizeFromPeriodTimeRule{
	SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
	{SNDRV_PCM_HW_PARAM_PERIOD_TIME, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &periodTime = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_TIME);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		auto constraint = utils::intervalMulDiv(periodTime, rate, usInSecond);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, constraint);
	}
};

Rule bufferSizeRule{
	SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
	{SNDRV_PCM_HW_PARAM_PERIODS, SNDRV_PCM_HW_PARAM_PERIOD_SIZE},
	[](Params &params) {
		auto &periods = params.interval(SNDRV_PCM_HW_PARAM_PERIODS);
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);

		auto constraint = utils::intervalMul(periods, periodSize);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE, constraint);
	}
};

Rule bufferSizeFromBufferBytesRule{
	SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
	{SNDRV_PCM_HW_PARAM_BUFFER_BYTES, SNDRV_PCM_HW_PARAM_FRAME_BITS},
	[](Params &params) {
		auto &bufferBytes = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);

		auto constraint = utils::intervalMulDiv(bufferBytes, 8, frameBits);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE, constraint);
	}
};

Rule bufferSizeFromBufferTimeRule{
	SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
	{SNDRV_PCM_HW_PARAM_BUFFER_TIME, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &bufferTime = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_TIME);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		auto constraint = utils::intervalMulDiv(bufferTime, rate, usInSecond);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE, constraint);
	}
};

Rule periodBytesRule{
	SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_FRAME_BITS},
	[](Params &params) {
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);

		auto constraint = utils::intervalMulDiv(periodSize, frameBits, 8);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES, constraint);
	}
};

Rule bufferBytesRule{
	SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_FRAME_BITS},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);

		auto constraint = utils::intervalMulDiv(bufferSize, frameBits, 8);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_BYTES, constraint);
	}
};

Rule periodTimeRule{
	SNDRV_PCM_HW_PARAM_PERIOD_TIME,
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		auto constraint = utils::intervalMulDiv(periodSize, usInSecond, rate);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_TIME, constraint);
	}
};

Rule bufferTimeRule{
	SNDRV_PCM_HW_PARAM_BUFFER_TIME,
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		auto constraint = utils::intervalMulDiv(bufferSize, usInSecond, rate);

		params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_TIME, constraint);
	}
};

std::array<Rule *, 19> commonRules{
	&sampleBitsRule,
	&sampleBitsFromFrameBitsRule,
	&frameBitsRule,
	&frameBitsFromPeriodBytesRule,
	&frameBitsFromBufferBytesRule,
	&channelsRule,
	&rateRule,
	&rateFromBufferSizeRule,
	&periodsRule,
	&periodSizeRule,
	&periodSizeFromPeriodBytesRule,
	&periodSizeFromPeriodTimeRule,
	&bufferSizeRule,
	&bufferSizeFromBufferBytesRule,
	&bufferSizeFromBufferTimeRule,
	&periodBytesRule,
	&bufferBytesRule,
	&periodTimeRule,
	&bufferTimeRule
};

} // namespace alsa::rules
