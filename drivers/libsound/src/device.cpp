#include "sound.hpp"
#include "rules.hpp"

#include <protocols/mbus/client.hpp>
#include <protocols/fs/server.hpp>

#include <async/recurring-event.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include <linux/types.h>
#include <sound/asound.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/time.h>

#include <print>
#include <functional>
#include <ctime>

namespace {

constexpr bool logRequests = true;
constexpr bool logWrites = false;

}

namespace alsa {

struct ApplyRulesResult {
	uint32_t cmask;
	bool invalid;
};

struct DeviceFile {
	static async::result<void>
	ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t sequence, int mask,
			async::cancellation_token cancellation);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	static async::result<helix::BorrowedDescriptor> accessMemory(void *object);

	static async::result<int> getFileFlags(void *object) {
		auto self = static_cast<DeviceFile *>(object);

		int flags = O_RDWR;
		if(self->nonBlock)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	static async::result<void> setFileFlags(void *object, int flags) {
		auto self = static_cast<DeviceFile *>(object);

		if(flags & ~O_NONBLOCK) {
			std::println(std::cout, "libsound: setFileFlags with unknown flags {:#x}", flags);
			co_return;
		}

		if(flags & O_NONBLOCK)
			self->nonBlock = true;
		else
		 	self->nonBlock = false;
	}

	static helix::UniqueLane serve(smarter::shared_ptr<DeviceFile> file);

	DeviceFile(sound::Device *device, bool non_block) : device{device}, nonBlock{non_block} {
		statusPage.update(eventSequence, 0);

		auto pageSize = getpagesize();

		size_t statusSize = (sizeof(snd_pcm_mmap_status) + pageSize - 1) & ~(pageSize - 1);
		size_t controlSize = (sizeof(snd_pcm_mmap_control) + pageSize - 1) & ~(pageSize - 1);

		HelHandle handle;
		HEL_CHECK(helAllocateMemory(SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW + controlSize, kHelAllocOnDemand, nullptr, &handle));

		memory = helix::UniqueDescriptor{handle};

		bufferMapping = helix::Mapping{memory, 0, SNDRV_PCM_MMAP_OFFSET_STATUS_OLD};
		statusMapping = helix::Mapping{memory, SNDRV_PCM_MMAP_OFFSET_STATUS, statusSize};
		controlMapping = helix::Mapping{memory, SNDRV_PCM_MMAP_OFFSET_CONTROL, controlSize};

		status = static_cast<snd_pcm_mmap_status *>(statusMapping.get());
		control = static_cast<snd_pcm_mmap_control *>(controlMapping.get());

		*status = {};
		*control = {};

		Rule hwChannelsRule{
			SNDRV_PCM_HW_PARAM_CHANNELS,
			{SNDRV_PCM_HW_PARAM_CHANNELS},
			[this](Params &params) {
				snd_interval constraint{};
				constraint.min = this->device->limits.channels.min;
				constraint.max = this->device->limits.channels.max;
				constraint.integer = 1;

				params.intersect_interval(SNDRV_PCM_HW_PARAM_CHANNELS, constraint);
			}
		};

		Rule hwRateRule{
			SNDRV_PCM_HW_PARAM_RATE,
			{SNDRV_PCM_HW_PARAM_RATE},
			[this](Params &params) {
				snd_interval constraint{};
				constraint.min = UINT32_MAX;
				constraint.integer = 1;

				for (auto rate : this->device->limits.sampleRates) {
					constraint.min = std::min(constraint.min, rate);
					constraint.max = std::max(constraint.max, rate);
				}

				params.intersect_interval(SNDRV_PCM_HW_PARAM_RATE, constraint);
			}
		};

		Rule hwPeriodBytesRule{
			SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
			{SNDRV_PCM_HW_PARAM_PERIOD_BYTES},
			[this](Params &params) {
				snd_interval constraint{};
				constraint.min = this->device->limits.periodSize.min;
				constraint.max = this->device->limits.periodSize.max;
				constraint.integer = 1;

				params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES, constraint);
			}
		};

		Rule hwPeriodsRule{
			SNDRV_PCM_HW_PARAM_PERIODS,
			{SNDRV_PCM_HW_PARAM_PERIODS},
			[this](Params &params) {
				snd_interval constraint{};
				constraint.min = this->device->limits.periodCount.min;
				constraint.max = this->device->limits.periodCount.max;
				constraint.integer = 1;

				params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIODS, constraint);
			}
		};

		Rule hwPeriodBytesAlignRule{
			SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
			{SNDRV_PCM_HW_PARAM_PERIOD_BYTES},
			[this](Params &params) {
				auto &periodBytes = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);

				auto constraint = utils::intervalStep(periodBytes, this->device->limits.periodSizeAlign);

				params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES, constraint);
			}
		};

		dynamicRules.push_back(std::move(hwChannelsRule));
		dynamicRules.push_back(std::move(hwRateRule));
		dynamicRules.push_back(std::move(hwPeriodBytesRule));
		dynamicRules.push_back(std::move(hwPeriodsRule));

		if (device->limits.forcePow2PeriodSizes) {
			Rule hwPeriodBytesPow2Rule{
				SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				{SNDRV_PCM_HW_PARAM_PERIOD_BYTES},
				[](Params &params) {
					auto &periodBytes = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);

					auto constraint = utils::intervalPow2(periodBytes);

					params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES, constraint);
				}
			};

			dynamicRules.push_back(std::move(hwPeriodBytesPow2Rule));
		}

		dynamicRules.push_back(std::move(hwPeriodBytesAlignRule));
	}

	snd_pcm_uframes_t getAvailableFrames() const {
		auto stream = device->attachedStream;

		snd_pcm_uframes_t available;
		if (stream->isCapture()) {
			available = status->hw_ptr - control->appl_ptr;
			if (static_cast<snd_pcm_sframes_t>(available) < 0)
				available += swParams.boundary;
		} else {
			available = bufferSizeFrames + status->hw_ptr - control->appl_ptr;
			if (available >= swParams.boundary)
				available -= swParams.boundary;
			else if (static_cast<snd_pcm_sframes_t>(available) < 0)
				available += swParams.boundary;
		}

		return available;
	}

	snd_pcm_uframes_t getHwDelayFrames() const {
		if (device->attachedStream->isCapture())
			return getAvailableFrames();
		else
			return bufferSizeFrames - getAvailableFrames();
	}

	ApplyRulesResult applyRules(Params &params, uint32_t rmask);

	void updatePosition();

	void periodCallback();

	async::result<frg::expected<protocols::fs::Error, size_t>> writeFrames(const std::vector<uint8_t> &data);

	sound::Device *device;
	helix::UniqueDescriptor memory;
	helix::Mapping bufferMapping;
	helix::Mapping statusMapping;
	helix::Mapping controlMapping;
	snd_pcm_mmap_status *status;
	snd_pcm_mmap_control *control;
	protocols::fs::StatusPageProvider statusPage;
	bool nonBlock;

	snd_pcm_hw_params hwParams{};
	snd_pcm_sw_params swParams{};

	uint64_t eventSequence{};
	async::recurring_event eventBell;

	uint32_t bufferSizeFrames{};
	uint32_t frameSize{};
	size_t lastPeriodPosition{};

	std::vector<Rule> dynamicRules;
};

ApplyRulesResult DeviceFile::applyRules(Params &params, uint32_t rmask) {
	ApplyRulesResult result{};

	while (true) {
		params.values.cmask = 0;

		for (auto rule : rules::commonRules) {
			if (rmask & rule->dependencies) {
				rule->func(params);

				if ((params.values.cmask & (1U << rule->what)) && params.interval(rule->what).empty) {
					std::println(std::cout, "libsound: interval {} is empty", rule->what);
					result.cmask |= params.values.cmask;
					result.invalid = true;
					return result;
				}
			}
		}

		for (auto rule : dynamicRules) {
			if (rmask & rule.dependencies) {
				rule.func(params);

				if ((params.values.cmask & (1U << rule.what)) && params.interval(rule.what).empty) {
					std::println(std::cout, "libsound: interval {} is empty", rule.what);
					result.cmask |= params.values.cmask;
					result.invalid = true;
					return result;
				}
			}
		}

		rmask = params.values.cmask;
		if (rmask == 0) {
			break;
		}
		result.cmask |= rmask;
	}

	return result;
}

void DeviceFile::updatePosition() {
	auto position = device->attachedStream->getPosition();
	assert(position);

	size_t delta;
	if (position.value() >= lastPeriodPosition)
		delta = position.value() - lastPeriodPosition;
	else
		delta = position.value() + (bufferSizeFrames * frameSize - lastPeriodPosition);

	lastPeriodPosition = position.value();

	snd_pcm_uframes_t hwPtr = status->hw_ptr;

	hwPtr += delta / frameSize;
	if (hwPtr >= swParams.boundary)
		hwPtr -= swParams.boundary;

	status->hw_ptr = hwPtr;

	if (swParams.tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
		switch (swParams.tstamp_type) {
		case SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY: {
			timeval tv{};
			gettimeofday(&tv, nullptr);
			TIMEVAL_TO_TIMESPEC(&tv, &status->tstamp);
			break;
		}
		case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC:
			clock_gettime(CLOCK_MONOTONIC, &status->tstamp);
			break;
		case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW:
			clock_gettime(CLOCK_MONOTONIC_RAW, &status->tstamp);
			break;
		default:
			break;
		}
	}

	auto result = async::run(pollStatus(this), helix::currentDispatcher);
	assert(result);
	int pollResult = std::get<1>(result.value());

	if (pollResult) {
		statusPage.update(++eventSequence, pollResult);
		eventBell.raise();
	}
}

void DeviceFile::periodCallback() {
	updatePosition();
}

async::result<frg::expected<protocols::fs::Error, size_t>> DeviceFile::writeFrames(const std::vector<uint8_t> &data) {
	if (!device->attachedStream)
		co_return protocols::fs::Error::illegalArguments;

	size_t totalFrames = data.size() / frameSize;
	size_t progress = 0;
	while (progress < totalFrames) {
		auto available = getAvailableFrames();

		auto applPtr = control->appl_ptr;
		auto realApplPtr = applPtr % bufferSizeFrames;
		size_t toCopy = std::min({available, totalFrames - progress, bufferSizeFrames - realApplPtr});

		if (toCopy == 0) {
			if (nonBlock)
				break;
			else {
				auto pollResult = co_await pollWait(this, eventSequence, 0, {});
				assert(pollResult);
			}
		}

		void *ptr = reinterpret_cast<void *>(uintptr_t(bufferMapping.get()) + realApplPtr * frameSize);
		memcpy(ptr, data.data() + progress * frameSize, toCopy * frameSize);

		applPtr += toCopy;
		if (applPtr >= swParams.boundary)
			applPtr -= swParams.boundary;

		control->appl_ptr = applPtr;

		available -= toCopy;

		auto stream = device->attachedStream;
		if (stream->isPaused && bufferSizeFrames - available >= swParams.start_threshold) {
			std::println(std::cout, "libsound: starting stream (trip threshold {})", swParams.start_threshold);
			auto streamStatus = stream->play();
			assert(streamStatus);
			status->state = SNDRV_PCM_STATE_RUNNING;
			stream->isPaused = false;
		}

		progress += toCopy;
	}

	if (progress)
		co_return progress;
	else
		co_return protocols::fs::Error::wouldBlock;
}

snd_mask soundFormatsToAlsa(const sound::DeviceLimits &limits) {
	snd_mask mask{};

	for (auto format : limits.formats) {
		switch (format) {
		case sound::Format::pcmS8:
			mask.bits[0] |= 1U << SNDRV_PCM_FORMAT_S8;
			break;
		case sound::Format::pcmS16:
			mask.bits[0] |= 1U << SNDRV_PCM_FORMAT_S16_LE;
			break;
		case sound::Format::pcmS20:
			mask.bits[0] |= 1U << SNDRV_PCM_FORMAT_S20_LE;
			break;
		case sound::Format::pcmS24:
			mask.bits[0] |= 1U << SNDRV_PCM_FORMAT_S24_LE;
			break;
		case sound::Format::pcmS32:
			mask.bits[0] |= 1U << SNDRV_PCM_FORMAT_S32_LE;
			break;
		}
	}

	return mask;
}

async::result<void> DeviceFile::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<DeviceFile *>(object);

	if (id != managarm::fs::GenericIoctlRequest::message_id) {
		std::println(std::cout, "libsound: unknown device ioctl() message with ID {}", id);
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
		co_return;
	}

	auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
	assert(req);
	if (req->command() == SNDRV_PCM_IOCTL_INFO) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query pcm info");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		snd_pcm_info info{};
		// TODO: get the device index somehow
		info.device = 0;
		info.subdevice = 0;
		info.stream = self->device->type == sound::DeviceType::playback ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE;
		// TODO: get the card index somehow
		info.card = 0;
		memcpy(info.id, "Managarm Sound Device", sizeof("Managarm Sound Device"));
		memcpy(info.name, "Managarm Sound Device", sizeof("Managarm Sound Device"));
		memcpy(info.subname, "subdevice #0", sizeof("subdevice #0"));
		info.dev_class = 0;
		info.dev_subclass = 0;
		info.subdevices_count = 1;
		info.subdevices_avail = 0;

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&info, sizeof(info))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_PVERSION) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query pcm version");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);
		resp.set_snd_pversion(SNDRV_PROTOCOL_VERSION(2, 0, 18));

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_USER_PVERSION) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm user pversion {} is ignored", req->snd_pcm_user_pversion());
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_TTSTAMP) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm ttstamp {}", req->snd_pcm_ttstamp());
		}

		self->swParams.tstamp_type = req->snd_pcm_ttstamp();
		self->swParams.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_REFINE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: refine pcm hw params");
		}

		Params params{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params.values, sizeof(params.values))
		);
		HEL_CHECK(recv_buffer.error());

		uint32_t rmask = params.values.rmask;
		uint32_t cmask = 0;
		bool invalid = false;

		auto applyHwMaskConstraint = [&](uint32_t what, uint32_t supported) {
			auto &value = params.mask(what);

			bool changed = (value.bits[0] & supported) != value.bits[0];

			value.bits[0] &= supported;

			for (size_t i = 1; i < sizeof(value.bits) / sizeof(*value.bits); i++) {
				changed |= value.bits[i] != 0;
				value.bits[i] = 0;
			}

			if (changed) {
				params.values.cmask |= 1U << what;
				auto applyResult = self->applyRules(params, what);
				cmask |= applyResult.cmask;
				invalid |= applyResult.invalid;
			}
		};

		if (rmask & (1U << SNDRV_PCM_HW_PARAM_ACCESS)) {
			constexpr uint32_t supported = (1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED)
					| (1U << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
			applyHwMaskConstraint(SNDRV_PCM_HW_PARAM_ACCESS, supported);
		}

		if ((rmask & (1U << SNDRV_PCM_HW_PARAM_FORMAT)) && !invalid) {
			auto supported = soundFormatsToAlsa(self->device->limits);
			applyHwMaskConstraint(SNDRV_PCM_HW_PARAM_FORMAT, supported.bits[0]);
		}

		if ((rmask & (1U << SNDRV_PCM_HW_PARAM_SUBFORMAT)) && !invalid) {
			constexpr uint32_t supported = (1U << SNDRV_PCM_SUBFORMAT_STD);
			applyHwMaskConstraint(SNDRV_PCM_HW_PARAM_SUBFORMAT, supported);
		}

		if (!invalid) {
			auto applyResult = self->applyRules(params, rmask);
			cmask |= applyResult.cmask;
			invalid = applyResult.invalid;
		}

		params.values.cmask = cmask;
		params.values.rmask = cmask;

		managarm::fs::GenericIoctlReply resp;

		if (invalid) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_PARAMS) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: set pcm hw params");
		}

		snd_pcm_hw_params params;

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params, sizeof(params))
		);
		HEL_CHECK(recv_buffer.error());

		// TODO: verify parameters

		self->hwParams = params;
		self->bufferSizeFrames = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
		self->frameSize = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_FRAME_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min / 8;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_SW_PARAMS) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: set pcm sw params");
		}

		snd_pcm_sw_params params;

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params, sizeof(params))
		);
		HEL_CHECK(recv_buffer.error());

		if (self->bufferSizeFrames != 0) {
			params.boundary = self->bufferSizeFrames;
			while (params.boundary * 2 <= LONG_MAX - self->bufferSizeFrames)
				params.boundary *= 2;
		}

		// TODO: verify parameters

		self->swParams = params;

		managarm::fs::GenericIoctlReply resp;

		if (self->bufferSizeFrames != 0)
			resp.set_error(managarm::fs::Errors::SUCCESS);
		else
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_DELAY) {
		if constexpr (logRequests) {
			// std::println(std::cout, "libsound: pcm delay");
		}

		snd_pcm_sframes_t delay{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&delay, sizeof(delay))
		);
		HEL_CHECK(recv_buffer.error());

		self->updatePosition();

		if (self->device->attachedStream) {
			delay = self->getHwDelayFrames();
		} else {
			delay = 0;
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&delay, sizeof(delay))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HWSYNC) {
		if constexpr (logRequests) {
			// std::println(std::cout, "libsound: pcm hwsync");
		}

		managarm::fs::GenericIoctlReply resp;

		if (self->device->attachedStream) {
			self->updatePosition();
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_STATUS_EXT) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm status ext");
		}

		snd_pcm_status status{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&status, sizeof(status))
		);
		HEL_CHECK(recv_buffer.error());

		status.state = self->status->state;
		status.tstamp = self->status->tstamp;
		status.appl_ptr = self->control->appl_ptr;
		status.hw_ptr = self->status->hw_ptr;

		if (self->status->state == SNDRV_PCM_STATE_RUNNING && self->device->attachedStream) {
			status.avail = self->getAvailableFrames();
			status.delay = self->getHwDelayFrames();
		} else {
			status.avail = 0;
			status.delay = 0;
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&status, sizeof(status))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_CHANNEL_INFO) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm channel info");
		}

		snd_pcm_channel_info channelInfo{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&channelInfo, sizeof(channelInfo))
		);
		HEL_CHECK(recv_buffer.error());

		uint32_t sampleBits = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_SAMPLE_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;

		channelInfo.offset = 0;
		channelInfo.first = channelInfo.channel * sampleBits;
		channelInfo.step = self->frameSize * 8;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&channelInfo, sizeof(channelInfo))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_PREPARE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm prepare");
		}

		if (self->status->state == SNDRV_PCM_STATE_OPEN || self->status->state == SNDRV_PCM_STATE_XRUN) {
			auto &fmtBits = self->hwParams.masks[SNDRV_PCM_HW_PARAM_FORMAT].bits;

			auto checkFmt = [&](snd_pcm_format_t fmt) {
				return fmtBits[fmt / 32] & 1U << (fmt % 32);
			};

			sound::Format fmt{};

			if (checkFmt(SNDRV_PCM_FORMAT_S8)) {
				fmt = sound::Format::pcmS8;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S16_LE)) {
				fmt = sound::Format::pcmS16;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S20_LE)) {
				fmt = sound::Format::pcmS20;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S24_LE)) {
				fmt = sound::Format::pcmS24;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S32_LE)) {
				fmt = sound::Format::pcmS32;
			} else {
				std::println(std::cout, "libsound: unsupported alsa format specified");
				assert(!"Unsupported format specified");
			}

			auto bufferSize = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_BUFFER_BYTES - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
			assert(bufferSize);
			auto periods = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
			auto periodBytes = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_PERIOD_BYTES - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
			assert(periodBytes);

			if constexpr (logRequests) {
				std::println(std::cout, "libsound: buffer size {} ({} periods * {})", bufferSize, periods, periodBytes);
			}

			std::vector<sound::PeriodChunk> periodChunks;
			for (size_t i = 0; i < bufferSize / periodBytes; i++) {
				void *virt = reinterpret_cast<void *>(uintptr_t(self->bufferMapping.get()) + i * periodBytes);
				sound::PeriodChunk chunk{
					.virt = virt,
					.phys = helix::ptrToPhysical(virt)
				};
				periodChunks.push_back(chunk);
			}

			sound::StreamParameters streamParams{
				.sampleRate = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
				.channels = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
				.periodCount = periods,
				.periodSize = periodBytes,
				.format = fmt,
				.periodCallback{[=] { self->periodCallback(); }},
				.periodChunks{std::move(periodChunks)}
			};

			if constexpr (logRequests) {
				std::println(std::cout, "libsound: sample rate {}, channels {}", streamParams.sampleRate, streamParams.channels);
			}

			sound::Stream *stream;
			if (self->device->type == sound::DeviceType::playback) {
				stream = self->device->card->findFreePlaybackStream();
				assert(stream);
			} else {
				stream = self->device->card->findFreeCaptureStream();
				assert(stream);
			}

			auto status = stream->setup(streamParams);
			assert(status);

			status = self->device->attachToStream(stream);
			assert(status);
			status = self->device->setVolume(100);
			assert(status);

			self->status->hw_ptr = 0;
			self->control->appl_ptr = 0;
			self->lastPeriodPosition = 0;
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		self->status->state = SNDRV_PCM_STATE_PREPARED;

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_START) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm start");
		}

		managarm::fs::GenericIoctlReply resp;

		auto stream = self->device->attachedStream;

		if (stream) {
			auto streamStatus = stream->play();
			assert(streamStatus);
			self->status->state = SNDRV_PCM_STATE_RUNNING;
			stream->isPaused = false;

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_XRUN) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm xrun");
		}

		managarm::fs::GenericIoctlReply resp;

		auto stream = self->device->attachedStream;

		if (stream) {
			auto status = stream->pause();
			assert(status);
			stream->isPaused = true;

			status = stream->stop();
			assert(status);
			stream->isReady = false;
			self->device->attachedStream = nullptr;

			self->status->state = SNDRV_PCM_STATE_XRUN;

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
		auto frames = req->snd_pcm_frames();

		if constexpr (logWrites) {
			std::println(std::cout, "libsound: pcm writei frames {}", frames);
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_snd_frame_size(self->frameSize);

		auto [send_info_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_info_resp.error());

		std::vector<uint8_t> data(frames * self->frameSize);

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(data.data(), data.size())
		);
		HEL_CHECK(recv_buffer.error());

		auto result = co_await self->writeFrames(data);

		if (result)
			resp.set_snd_result(result.value());
		else
			resp.set_error(result.error() | protocols::fs::toFsError);

		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_DROP) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm drop");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_result(0);

		if (self->status->state != SNDRV_PCM_STATE_OPEN) {
			if (self->device->attachedStream) {
				auto stream = self->device->attachedStream;
				auto status = stream->pause();
				assert(status);
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_FREE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm hw free");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		if (auto stream = self->device->attachedStream) {
			auto status = stream->pause();
			assert(status);
			stream->isPaused = true;
			status = stream->stop();
			assert(status);
			stream->isReady = false;
			self->device->attachedStream = nullptr;
		}

		self->status->state = SNDRV_PCM_STATE_OPEN;

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else {
		std::println(std::cout, "Unknown ioctl() message with ID {}", req->command());
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
DeviceFile::pollWait(void *object, uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	(void) mask;
	(void) cancellation;

	auto self = static_cast<DeviceFile *>(object);

	if(sequence > self->eventSequence)
		co_return protocols::fs::Error::illegalArguments;

	// Wait until we surpass the input sequence.
	while(sequence == self->eventSequence)
		co_await self->eventBell.async_wait_if([&] { return sequence == self->eventSequence; });

	auto result = co_await pollStatus(object);
	assert(result);

	co_return protocols::fs::PollWaitResult{self->eventSequence, std::get<1>(result.value())};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
DeviceFile::pollStatus(void *object) {
	auto self = static_cast<DeviceFile *>(object);

	auto available = self->getAvailableFrames();

	auto stream = self->device->attachedStream;
	int pollSuccess = stream->isCapture() ? (EPOLLIN | EPOLLRDNORM) : (EPOLLOUT | EPOLLWRNORM);

	int s = 0;

	switch (self->status->state) {
	case SNDRV_PCM_STATE_PREPARED:
	case SNDRV_PCM_STATE_RUNNING:
	case SNDRV_PCM_STATE_PAUSED:
		if (available >= self->control->avail_min)
			s |= pollSuccess;
		break;
	case SNDRV_PCM_STATE_DRAINING:
		if (stream->isCapture()) {
			s |= pollSuccess;
			if (available == 0)
				s |= EPOLLERR;
		}
		break;
	default:
		s |= EPOLLERR;
		break;
	}

	co_return protocols::fs::PollStatusResult{self->eventSequence, s};
}

async::result<helix::BorrowedDescriptor> DeviceFile::accessMemory(void *object) {
	auto self = static_cast<DeviceFile *>(object);
	co_return self->memory;
}

constexpr auto fileOperations = protocols::fs::FileOperations{
	.accessMemory = &DeviceFile::accessMemory,
	.ioctl = &DeviceFile::ioctl,
	.pollWait = &DeviceFile::pollWait,
	.pollStatus = &DeviceFile::pollStatus,
	.getFileFlags = &DeviceFile::getFileFlags,
	.setFileFlags = &DeviceFile::setFileFlags
};

helix::UniqueLane DeviceFile::serve(smarter::shared_ptr<DeviceFile> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(
			std::move(local_lane), file, &fileOperations));
	return remote_lane;
}

async::detached serveDevice(sound::Device *device,
		helix::UniqueLane lane) {
	std::cout << "alsa device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		recv_req.reset();
		if (req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			auto file = smarter::make_shared<DeviceFile>(device,
					req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK);
			auto remote_lane = DeviceFile::serve(file);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FileCaps::FC_STATUS_PAGE);

			auto [send_resp, push_pt, push_page] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remote_lane),
				helix_ng::pushDescriptor(file->statusPage.getMemory())
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		} else {
			throw std::runtime_error("Invalid serveDevice request!");
		}
	}
}

} // namespace alsa

namespace sound {

async::detached runDevice(Device *device) {
	mbus_ng::Properties soundDeviceDescriptor{
		{"unix.subsystem", mbus_ng::StringItem{"sound"}},
		{"drvcore.mbus-parent", mbus_ng::StringItem(std::to_string(device->parentId))},
		{"sound.type", mbus_ng::StringItem{device->type == sound::DeviceType::playback ? "playback" : "capture"}},
		{"class", mbus_ng::StringItem{"sound-device"}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
			"sound-device", soundDeviceDescriptor)).unwrap();

	std::println(std::cout, "libsound: Serving device");

	while (true) {
		auto [localLane, remoteLane] = helix::createStream();

		// If this fails, too bad!
		(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

		alsa::serveDevice(device, std::move(localLane));
	}
}

} // namespace sound
