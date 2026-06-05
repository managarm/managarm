#include "sound.hpp"

#include <protocols/mbus/client.hpp>
#include <protocols/fs/server.hpp>

#include <async/recurring-event.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include <linux/types.h>
#include <sound/asound.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

#include <print>

namespace {

constexpr bool logRequests = true;

}

namespace alsa {

struct CardFile {
	static async::result<void>
	ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation);

	static helix::UniqueLane serve(smarter::shared_ptr<CardFile> file);

	CardFile(sound::Card *card) : card{card} { }

	sound::Card *card;
};

namespace {

constexpr char cardId[] = "PCH";
constexpr char driverName[] = "HDA-Intel";
constexpr char cardName[] = "HDA Intel PCH";

}

async::result<void> CardFile::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<CardFile *>(object);

	if (id != managarm::fs::GenericIoctlRequest::message_id) {
		std::println(std::cout, "libsound: unknown control ioctl() message with ID {}", id);
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
		co_return;
	}

	auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
	assert(req);
	if (req->command() == SNDRV_CTL_IOCTL_CARD_INFO) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query card info");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		snd_ctl_card_info info{};
		// TODO: get the card index somehow
		info.card = 0;
		memcpy(info.id, cardId, sizeof(cardId));
		memcpy(info.driver, driverName, sizeof(driverName));
		memcpy(info.name, cardName, sizeof(cardName));
		memcpy(info.longname, cardName, sizeof(cardName));

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&info, sizeof(info))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_CTL_IOCTL_PVERSION) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query version");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);
		resp.set_snd_pversion(SNDRV_PROTOCOL_VERSION(2, 0, 9));

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm prefer subdevice is ignored");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm next device");
		}

		managarm::fs::GenericIoctlReply resp;

		auto current_dev = req->snd_device();
		if (current_dev + 1 < static_cast<int>(self->card->devices.size())) {
			resp.set_snd_device(current_dev + 1);
		} else {
			resp.set_snd_device(-1);
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_CTL_IOCTL_PCM_INFO) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query pcm info");
		}

		snd_pcm_info info{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&info, sizeof(info))
		);
		HEL_CHECK(recv_buffer.error());

		// device + stream + subdevice are filled
		// TODO: get the card index somehow
		info.card = 0;
		strlcpy(reinterpret_cast<char *>(info.id), "uHDA", sizeof(info.id));
		strlcpy(reinterpret_cast<char *>(info.name), "uHDA", sizeof(info.name));
		strlcpy(reinterpret_cast<char *>(info.subname), "subdevice #0", sizeof(info.subname));
		info.dev_class = 0;
		info.dev_subclass = 0;
		info.subdevices_count = 1;
		info.subdevices_avail = 0;

		managarm::fs::GenericIoctlReply resp;

		if (info.stream == SNDRV_PCM_STREAM_CAPTURE) {
			resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&info, sizeof(info))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else {
		std::println(std::cout, "libsound: unknown control ioctl() message with ID {}", req->command());
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

constexpr auto fileOperations = protocols::fs::FileOperations{
	.ioctl = &CardFile::ioctl
};

helix::UniqueLane CardFile::serve(smarter::shared_ptr<CardFile> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(
			std::move(local_lane), file, &fileOperations));
	return remote_lane;
}

async::detached serveCard(sound::Card *card,
		helix::UniqueLane lane) {
	std::cout << "alsa card: Connection" << std::endl;

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
			auto file = smarter::make_shared<CardFile>(card);
			auto remote_lane = CardFile::serve(file);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_pt] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
		} else {
			throw std::runtime_error("Invalid serveCard request!");
		}
	}
}

} // namespace alsa

namespace sound {

async::detached runCard(Card *card, uint64_t numDevices) {
	mbus_ng::Properties soundCardDescriptor{
		{"unix.subsystem", mbus_ng::StringItem{"sound"}},
		{"class", mbus_ng::StringItem{"sound-card"}},
		{"sound.num-devices", mbus_ng::StringItem{std::to_string(numDevices)}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
			"sound-card", soundCardDescriptor)).unwrap();

	card->mbusId = entity.id();
	card->mbusPublishedEvent.raise();

	std::println(std::cout, "libsound: Serving card");

	while (true) {
		auto [localLane, remoteLane] = helix::createStream();

		// If this fails, too bad!
		(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

		alsa::serveCard(card, std::move(localLane));
	}
}

} // namespace sound
