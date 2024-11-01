#include <asm/ioctls.h>
#include <numeric>
#include <signal.h>
#include <sstream>
#include <sys/epoll.h>
#include <termios.h>

#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>

#include "core/tty.hpp"
#include "file.hpp"
#include "fs.bragi.hpp"
#include "process.hpp"
#include "pts.hpp"

#include <bitset>

namespace pts {

namespace {

struct MasterFile;
struct RootLink;
struct DeviceNode;
struct RootNode;

bool logReadWrite = false;
bool logAttrs = false;

int nextPtsIndex = 0;

extern std::shared_ptr<RootLink> globalRootLink;

//-----------------------------------------------------------------------------

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	size_t offset = 0;
};

struct Channel {
	Channel(int pts_index) : ptsIndex{pts_index}, currentSeq{1}, masterInSeq{0}, slaveInSeq{0} {
		memset(&activeSettings, 0, sizeof(struct termios));
		// cflag: Linux also stores a baud rate here.
		// lflag: Linux additionally sets ECHOCTL, ECHOKE (which we do not have).
		activeSettings.c_iflag = ICRNL | IXON;
		activeSettings.c_oflag = OPOST | ONLCR;
		activeSettings.c_cflag = CS8 | CREAD | HUPCL;
		activeSettings.c_lflag = TTYDEF_LFLAG | ECHOK;
		activeSettings.c_cc[VINTR] = CINTR;
		activeSettings.c_cc[VEOF] = CEOF;
		activeSettings.c_cc[VKILL] = CKILL;
		activeSettings.c_cc[VSTART] = CSTART;
		activeSettings.c_cc[VSTOP] = CSTOP;
		activeSettings.c_cc[VSUSP] = CSUSP;
		activeSettings.c_cc[VQUIT] = CQUIT;
		activeSettings.c_cc[VERASE] = CERASE; // DEL character.
		activeSettings.c_cc[VMIN] = CMIN;
		activeSettings.c_cc[VDISCARD] = CDISCARD;
		activeSettings.c_cc[VLNEXT] = CLNEXT;
		activeSettings.c_cc[VWERASE] = CWERASE;
		activeSettings.c_cc[VREPRINT] = CRPRNT;
		cfsetispeed(&activeSettings, B38400);
		cfsetospeed(&activeSettings, B38400);
	}

	async::result<void> commonIoctl(
	    Process *process,
	    uint32_t id,
	    helix_ng::RecvInlineResult msg,
	    helix::UniqueLane conversation
	);

	int ptsIndex;
	ControllingTerminalState cts;

	struct termios activeSettings;

	int width = 80;
	int height = 25;
	int pixelWidth = 8 * 80;
	int pixelHeight = 16 * 25;

	// Status management for poll().
	async::recurring_event statusBell;
	uint64_t currentSeq;
	uint64_t masterInSeq;
	uint64_t slaveInSeq;

	// The actual queue of this pipe.
	std::deque<Packet> masterQueue;
	std::deque<Packet> slaveQueue;
};

namespace {

void processOut(const char c, Packet &packet, std::shared_ptr<Channel> channel) {
	if (!(channel->activeSettings.c_oflag & OPOST)) {
		packet.buffer.push_back(c);
		return;
	}

	if ((channel->activeSettings.c_oflag & ONLCR) && c == '\n') {
		packet.buffer.push_back('\r');
		packet.buffer.push_back('\n');
		return;
	}

	packet.buffer.push_back(c);

	return;
}

void processIn(const char character, Packet &packet, std::shared_ptr<Channel> channel) {
	auto enqueuePacket = [&channel](Packet packet) {
		channel->slaveQueue.push_back(std::move(packet));
		channel->slaveInSeq = ++channel->currentSeq;
		channel->statusBell.raise();
	};

	auto enqueueOut = [&channel](Packet packet) {
		Packet parsed{};

		for (auto c : packet.buffer) {
			processOut(c, parsed, channel);
		}

		channel->masterQueue.push_back(std::move(parsed));
		channel->masterInSeq = ++channel->currentSeq;
		channel->statusBell.raise();
	};

	auto is_control_char = [](char c) -> bool { return c < 32 || c == 0x7F; };

	auto erase_char = [&](bool erase) {
		if (!packet.buffer.empty()) {
			size_t chars = 1;
			char c = packet.buffer.back();
			packet.buffer.pop_back();

			if (is_control_char(c))
				chars = 2;

			if ((channel->activeSettings.c_lflag & ECHO) && erase) {
				Packet echopacket;
				for (size_t i = 0; i < chars; i++) {
					echopacket.buffer.push_back('\b');
					echopacket.buffer.push_back(' ');
					echopacket.buffer.push_back('\b');
				}
				enqueueOut(std::move(echopacket));
			}
		}
	};

	char c = character;

	if (channel->activeSettings.c_iflag & ISTRIP)
		c &= 0x7F;

	if (c == '\r') {
		if (channel->activeSettings.c_iflag & IGNCR)
			return;

		if (channel->activeSettings.c_iflag & ICRNL)
			c = '\n';
	} else if (c == '\n') {
		if (channel->activeSettings.c_iflag & INLCR)
			c = '\r';
	}

	if ((channel->activeSettings.c_iflag & IUCLC) && (c >= 'A' && c <= 'Z'))
		c = c - 'A' + 'a';

	if (channel->activeSettings.c_lflag & ISIG) {
		std::optional<int> signal = {};

		if (c == static_cast<char>(channel->activeSettings.c_cc[VINTR])) {
			signal = SIGINT;
		} else if (c == static_cast<char>(channel->activeSettings.c_cc[VQUIT])) {
			signal = SIGQUIT;
		} else if (c == static_cast<char>(channel->activeSettings.c_cc[VSUSP])) {
			signal = SIGTSTP;
		}

		if (signal.has_value()) {
			UserSignal info;
			channel->cts.issueSignalToForegroundGroup(signal.value(), info);
			return;
		}
	}

	if (channel->activeSettings.c_lflag & ICANON) {
		if (c == static_cast<char>(channel->activeSettings.c_cc[VKILL])) {
			while (!packet.buffer.empty()) {
				erase_char(channel->activeSettings.c_lflag & ECHOK);
			}

			return;
		}

		if (c == static_cast<char>(channel->activeSettings.c_cc[VERASE])) {
			erase_char(channel->activeSettings.c_lflag & ECHOE);
			return;
		}

		if ((channel->activeSettings.c_lflag & IEXTEN) &&
		    c == static_cast<char>(channel->activeSettings.c_cc[VWERASE])) {
			// remove trailing whitespace
			while (!packet.buffer.empty() && packet.buffer.back() == ' ') {
				erase_char(channel->activeSettings.c_lflag & ECHOE);
			}

			// remove last word
			while (!packet.buffer.empty() && packet.buffer.back() != ' ') {
				erase_char(channel->activeSettings.c_lflag & ECHOE);
			}

			return;
		}

		if (c == static_cast<char>(channel->activeSettings.c_cc[VEOF])) {
			enqueuePacket(std::move(packet));
			packet = Packet{};

			return;
		}
	}

	char echo_char = (channel->activeSettings.c_lflag & ECHO) ? c : '\0';

	if ((channel->activeSettings.c_lflag & ECHOCTL) && (channel->activeSettings.c_lflag & ECHO) &&
	    c < 32 && c != '\n' && c != '\t') {
		Packet echopacket;
		echopacket.buffer.push_back('^');
		echopacket.buffer.push_back(c + 0x40);
		enqueueOut(std::move(echopacket));
		echo_char = '\0';
	}

	if (channel->activeSettings.c_lflag & ICANON) {
		packet.buffer.push_back(c);

		if (echo_char) {
			Packet echopacket;
			if (is_control_char(c) && c != '\n') {
				echopacket.buffer.push_back('^');
				echopacket.buffer.push_back(('@' + c) % 128);
			} else {
				echopacket.buffer.push_back(c);
			}
			enqueueOut(std::move(echopacket));
		}

		if (c == '\n' || c == static_cast<char>(channel->activeSettings.c_cc[VEOL]) ||
		    c == static_cast<char>(channel->activeSettings.c_cc[VEOL2])) {
			if (!(channel->activeSettings.c_lflag & ECHO) &&
			    (channel->activeSettings.c_lflag & ECHONL)) {
				Packet echopacket;
				echopacket.buffer.push_back(c);
				enqueueOut(std::move(echopacket));
			}
			enqueuePacket(std::move(packet));
			packet = Packet{};
			return;
		}

		return;
	} else if (channel->activeSettings.c_lflag & ECHO) {
		Packet echopacket;
		echopacket.buffer.push_back(c);
		enqueueOut(std::move(echopacket));
	}

	// Not a special character. Emit to the slave.
	packet.buffer.push_back(c);

	return;
}

} // namespace

//-----------------------------------------------------------------------------
// Device and file structs.
//-----------------------------------------------------------------------------

struct MasterDevice final : UnixDevice {
	MasterDevice() : UnixDevice(VfsType::charDevice) { assignId({5, 2}); }

	std::string nodePath() override { return "ptmx"; }

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(
	    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags
	) override;
};

struct SlaveDevice final : UnixDevice {
	SlaveDevice(std::shared_ptr<Channel> channel);

	std::string nodePath() override { return std::string{}; }

	async::result<frg::expected<Error, SharedFilePtr>> open(
	    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags
	) override;

  private:
	std::shared_ptr<Channel> _channel;
};

struct MasterFile final : File {
  public:
	static void serve(smarter::shared_ptr<MasterFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane), file, &File::fileOperations)
		);
	}

	MasterFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, bool nonBlocking);

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t maxLength) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, ControllingTerminalState *>>
	getControllingTerminal() override;

	async::result<frg::expected<Error, PollWaitResult>> pollWait(
	    Process *, uint64_t sequence, int mask, async::cancellation_token cancellation
	) override;

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override;

	async::result<void> ioctl(
	    Process *process,
	    uint32_t id,
	    helix_ng::RecvInlineResult msg,
	    helix::UniqueLane conversation
	) override;

	async::result<void> setFileFlags(int flags) override {
		if (flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on pty \e[1;34m" << structName()
			          << "\e[0m called with unknown flags" << std::endl;
			co_return;
		}
		if (flags & O_NONBLOCK)
			_nonBlocking = true;
		else
			_nonBlocking = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		if (_nonBlocking)
			co_return O_NONBLOCK;
		co_return 0;
	}

	helix::BorrowedDescriptor getPassthroughLane() override { return _passthrough; }

  private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;
	Packet _packet{};

	bool _nonBlocking;
};

struct SlaveFile final : File {
  public:
	static void serve(smarter::shared_ptr<SlaveFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane), file, &File::fileOperations)
		);
	}

	SlaveFile(
	    std::shared_ptr<MountView> mount,
	    std::shared_ptr<FsLink> link,
	    std::shared_ptr<Channel> channel,
	    bool nonBlock
	);

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t maxLength) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, ControllingTerminalState *>>
	getControllingTerminal() override;

	async::result<frg::expected<Error, PollWaitResult>> pollWait(
	    Process *, uint64_t sequence, int mask, async::cancellation_token cancellation
	) override;

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override;

	async::result<void> ioctl(
	    Process *process,
	    uint32_t id,
	    helix_ng::RecvInlineResult msg,
	    helix::UniqueLane conversation
	) override;

	async::result<int> getFileFlags() override {
		if (nonBlock_)
			co_return O_NONBLOCK;
		co_return 0;
	}

	helix::BorrowedDescriptor getPassthroughLane() override { return _passthrough; }

	async::result<frg::expected<Error, std::string>> ttyname() override;

  private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;
	Packet _packet{};

	bool nonBlock_;
};

//-----------------------------------------------------------------------------
// File system structs.
//-----------------------------------------------------------------------------

struct Link final : FsLink {
  public:
	explicit Link(RootNode *root, std::string name, std::shared_ptr<DeviceNode> device)
	    : _root{root},
	      _name{std::move(name)},
	      _device{std::move(device)} {}

	std::shared_ptr<FsNode> getOwner() override;

	std::string getName() override;

	std::shared_ptr<FsNode> getTarget() override;

  private:
	RootNode *_root;
	std::string _name;
	std::shared_ptr<DeviceNode> _device;
};

struct RootLink final : FsLink {
	RootLink();

	RootNode *rootNode() { return _root.get(); }

	std::shared_ptr<FsNode> getOwner() override {
		throw std::logic_error("posix: pts RootLink has no owner");
	}

	std::string getName() override { throw std::logic_error("posix: pts RootLink has no name"); }

	std::shared_ptr<FsNode> getTarget() override;

  private:
	std::shared_ptr<RootNode> _root;
};

struct LinkCompare {
	struct is_transparent {};

	bool operator()(const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}
	bool operator()(const std::string &name, const std::shared_ptr<Link> &link) const {
		return name < link->getName();
	}

	bool operator()(const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
		return a->getName() < b->getName();
	}
};

struct DeviceNode final : FsNode {
  public:
	DeviceNode(DeviceId id) : _type{VfsType::charDevice}, _id{id} {}

	VfsType getType() override { return _type; }

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts DeviceNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}

	DeviceId readDevice() override { return _id; }

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(
	    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags
	) override {
		return openDevice(_type, _id, std::move(mount), std::move(link), semantic_flags);
	}

  private:
	VfsType _type;
	DeviceId _id;
};

struct RootNode final : FsNode, std::enable_shared_from_this<RootNode> {
	friend struct Superblock;
	friend struct DirectoryFile;

  public:
	VfsType getType() override { return VfsType::directory; }

	void linkDevice(std::string name, std::shared_ptr<DeviceNode> node) {
		auto link = std::make_shared<Link>(this, name, std::move(node));
		_entries.insert(std::move(link));
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts RootNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name
	) override {
		auto it = _entries.find(name);
		if (it != _entries.end())
			co_return *it;
		co_return nullptr; // TODO: Return an error code.
	}

  private:
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

async::result<void> Channel::commonIoctl(
    Process *, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation
) {
	if (id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
		assert(req);

		if (req->command() == TIOCSCTTY) {
			auto [extractCreds] =
			    co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(extractCreds.error());

			auto process = findProcessWithCredentials(extractCreds.credentials());

			managarm::fs::GenericIoctlReply resp;
			if (auto e = cts.assignSessionOf(process.get()); e == Error::illegalArguments) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else if (e == Error::insufficientPermissions) {
				resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
			} else {
				assert(e == Error::success);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto [sendResp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(sendResp.error());
		} else if (req->command() == TIOCGPGRP) {
			managarm::fs::GenericIoctlReply resp;

			auto [extractCreds] =
			    co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(extractCreds.error());

			auto process = findProcessWithCredentials(extractCreds.credentials());

			if (&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
				resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
			} else {
				resp.set_pid(cts.getSession()->getForegroundGroup()->getHull()->getPid());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCSPGRP) {
			managarm::fs::GenericIoctlReply resp;

			auto [extractCreds] =
			    co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(extractCreds.error());

			auto process = findProcessWithCredentials(extractCreds.credentials());
			auto group = process->pgPointer()->getSession()->getProcessGroupById(req->pgid());
			if (!group) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				Error ret = cts.getSession()->setForegroundGroup(group.get());
				if (ret == Error::insufficientPermissions) {
					resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
				} else {
					assert(ret == Error::success);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				}
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCGSID) {
			managarm::fs::GenericIoctlReply resp;

			auto [extractCreds] =
			    co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(extractCreds.error());

			auto process = findProcessWithCredentials(extractCreds.credentials());

			if (&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
				resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
			} else {
				resp.set_pid(cts.getSession()->getSessionId());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			std::cout << "\e[31m"
			             "posix: Rejecting unknown PTS ioctl (commonIoctl) "
			          << req->command() << "\e[39m" << std::endl;
		}
	} else {
		std::cout << "\e[31m"
		             "posix: Rejecting unknown PTS ioctl message (commonIoctl) "
		          << id << "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// MasterDevice implementation.
//-----------------------------------------------------------------------------

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> MasterDevice::open(
    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags
) {
	if (semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)) {
		std::cout << "\e[31mposix: open() received illegal arguments:"
		          << std::bitset<32>(semantic_flags)
		          << "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are "
		             "allowed.\e[39m"
		          << std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<MasterFile>(
	    std::move(mount), std::move(link), semantic_flags & semanticNonBlock
	);
	file->setupWeakFile(file);
	MasterFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

MasterFile::MasterFile(
    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, bool nonBlocking
)
    : File{StructName::get("pts.master"), std::move(mount), std::move(link), File::defaultPipeLikeSeek},
      _channel{std::make_shared<Channel>(nextPtsIndex++)},
      _nonBlocking{nonBlocking} {
	auto slave_device = std::make_shared<SlaveDevice>(_channel);
	charRegistry.install(std::move(slave_device));

	globalRootLink->rootNode()->linkDevice(
	    std::to_string(_channel->ptsIndex),
	    std::make_shared<DeviceNode>(DeviceId{136, _channel->ptsIndex})
	);
}

async::result<frg::expected<Error, size_t>>
MasterFile::readSome(Process *, void *data, size_t maxLength) {
	if (logReadWrite)
		std::cout << std::format("posix: Read from tty {}\n", structName());
	if (!maxLength)
		co_return 0;

	if (_channel->masterQueue.empty() && _nonBlocking)
		co_return Error::wouldBlock;

	while (_channel->masterQueue.empty())
		co_await _channel->statusBell.async_wait();

	auto packet = &_channel->masterQueue.front();
	size_t chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	assert(chunk); // Otherwise, we return above due to !maxLength.
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if (packet->offset == packet->buffer.size())
		_channel->masterQueue.pop_front();
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MasterFile::writeAll(Process *, const void *data, size_t length) {
	if (logReadWrite)
		std::cout << std::format("posix: Write to tty {} of size {}\n", structName(), length);

	auto enqueuePacket = [this](Packet packet) {
		_channel->slaveQueue.push_back(std::move(packet));
		_channel->slaveInSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
	};

	auto s = reinterpret_cast<const char *>(data);
	for (size_t i = 0; i < length; i++) {
		processIn(s[i], _packet, _channel);
	}

	// Check whether all data was discarded above.
	if (!(_channel->activeSettings.c_lflag & ICANON)) {
		enqueuePacket(std::move(_packet));
		_packet = Packet{};
	}

	co_return length;
}

async::result<frg::expected<Error, ControllingTerminalState *>>
MasterFile::getControllingTerminal() {
	co_return &_channel->cts;
}

async::result<frg::expected<Error, PollWaitResult>> MasterFile::pollWait(
    Process *, uint64_t past_seq, int mask, async::cancellation_token cancellation
) {
	(void)mask; // TODO: utilize mask.
	assert(past_seq <= _channel->currentSeq);

	while (past_seq == _channel->currentSeq && !cancellation.is_cancellation_requested())
		co_await _channel->statusBell.async_wait(cancellation);

	// For now making pts files always writable is sufficient.
	int edges = EPOLLOUT;
	if (_channel->masterInSeq > past_seq)
		edges |= EPOLLIN;

	co_return PollWaitResult{_channel->currentSeq, edges};
}

async::result<frg::expected<Error, PollStatusResult>> MasterFile::pollStatus(Process *) {
	// For now making pts files always writable is sufficient.
	int events = EPOLLOUT;
	if (!_channel->masterQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

async::result<void> MasterFile::ioctl(
    Process *process, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation
) {
	if (id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
		assert(req);

		if (req->command() == TIOCGPTN) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_index(_channel->ptsIndex);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCSWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			if (logAttrs)
				std::cout << "posix: PTS window size is now " << req->pts_width() << "x"
				          << req->pts_height() << " chars, " << req->pts_pixel_width() << "x"
				          << req->pts_pixel_height() << " pixels (set by master)" << std::endl;

			_channel->width = req->pts_width();
			_channel->height = req->pts_height();
			_channel->pixelWidth = req->pts_pixel_width();
			_channel->pixelHeight = req->pts_pixel_height();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			// XXX: This should deliver SIGWINCH to the parent under certain conditions
			UserSignal info;
			_channel->cts.issueSignalToForegroundGroup(SIGWINCH, info);
		} else if (req->command() == FIONREAD) {
			managarm::fs::GenericIoctlReply resp;

			size_t count = std::transform_reduce(
			    _channel->masterQueue.begin(),
			    _channel->masterQueue.end(),
			    size_t{0},
			    std::plus<>(),
			    [](const Packet &p) { return p.buffer.size() - p.offset; }
			);

			resp.set_fionread_count(count);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCSCTTY || req->command() == TIOCGPGRP ||
		           req->command() == TIOCSPGRP || req->command() == TIOCGSID) {
			co_await _channel->commonIoctl(process, id, std::move(msg), std::move(conversation));
		} else {
			std::cout << "\e[31m"
			             "posix: Rejecting unknown PTS master ioctl "
			          << req->command() << "\e[39m" << std::endl;
		}
	} else {
		std::cout << "\e[31m"
		             "posix: Rejecting unknown PTS master ioctl message "
		          << id << "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// SlaveDevice implementation.
//-----------------------------------------------------------------------------

SlaveDevice::SlaveDevice(std::shared_ptr<Channel> channel)
    : UnixDevice(VfsType::charDevice),
      _channel{std::move(channel)} {
	assignId({136, _channel->ptsIndex});
}

async::result<frg::expected<Error, SharedFilePtr>> SlaveDevice::open(
    std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags
) {
	if (semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)) {
		std::cout << "\e[31mposix: open() received illegal arguments:"
		          << std::bitset<32>(semantic_flags)
		          << "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are "
		             "allowed.\e[39m"
		          << std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<SlaveFile>(
	    std::move(mount), std::move(link), _channel, semantic_flags & semanticNonBlock
	);
	file->setupWeakFile(file);
	SlaveFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

SlaveFile::SlaveFile(
    std::shared_ptr<MountView> mount,
    std::shared_ptr<FsLink> link,
    std::shared_ptr<Channel> channel,
    bool nonBlock
)
    : File{StructName::get("pts.slave"), std::move(mount), std::move(link), File::defaultIsTerminal | File::defaultPipeLikeSeek},
      _channel{std::move(channel)},
      nonBlock_{nonBlock} {}

async::result<frg::expected<Error, size_t>>
SlaveFile::readSome(Process *, void *data, size_t maxLength) {
	if (logReadWrite)
		std::cout << "posix: Read from tty " << structName() << std::endl;
	if (!maxLength)
		co_return 0;

	while (_channel->slaveQueue.empty()) {
		if (nonBlock_) {
			if (logReadWrite)
				std::cout << "posix: tty would block" << std::endl;
			co_return Error::wouldBlock;
		}
		co_await _channel->statusBell.async_wait();
	}

	auto packet = &_channel->slaveQueue.front();
	auto chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	if (chunk)
		memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if (packet->offset == packet->buffer.size())
		_channel->slaveQueue.pop_front();
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
SlaveFile::writeAll(Process *, const void *data, size_t length) {
	if (logReadWrite)
		std::cout << std::format("posix: Write to tty {}\n", structName());

	if (!length)
		co_return {};

	// Perform output processing.
	for (size_t i = 0; i < length; i++) {
		char c;
		memcpy(&c, reinterpret_cast<const char *>(data) + i, 1);
		processOut(c, _packet, _channel);
	}

	_channel->masterQueue.push_back(std::move(_packet));
	_channel->masterInSeq = ++_channel->currentSeq;
	_channel->statusBell.raise();
	_packet = Packet{};
	co_return length;
}

async::result<frg::expected<Error, ControllingTerminalState *>>
SlaveFile::getControllingTerminal() {
	co_return &_channel->cts;
}

async::result<frg::expected<Error, PollWaitResult>> SlaveFile::pollWait(
    Process *, uint64_t past_seq, int mask, async::cancellation_token cancellation
) {
	(void)mask; // TODO: utilize mask.
	assert(past_seq <= _channel->currentSeq);
	while (past_seq == _channel->currentSeq && !cancellation.is_cancellation_requested())
		co_await _channel->statusBell.async_wait(cancellation);

	// For now making pts files always writable is sufficient.
	int edges = EPOLLOUT;
	if (_channel->slaveInSeq > past_seq)
		edges |= EPOLLIN;

	co_return PollWaitResult{_channel->currentSeq, edges};
}

async::result<frg::expected<Error, PollStatusResult>> SlaveFile::pollStatus(Process *) {
	// For now making pts files always writable is sufficient.
	int events = EPOLLOUT;
	if (!_channel->slaveQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

async::result<void> SlaveFile::ioctl(
    Process *process, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation
) {
	if (id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);

		if (req->command() == TCGETS) {
			managarm::fs::GenericIoctlReply resp;
			struct termios attrs;

			memset(&attrs, 0, sizeof(struct termios));
			ttyCopyTermios(_channel->activeSettings, attrs);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(
			    conversation,
			    helix_ng::sendBuffer(ser.data(), ser.size()),
			    helix_ng::sendBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_attrs.error());
		} else if (req->command() == TCSETS) {
			struct termios attrs;
			managarm::fs::GenericIoctlReply resp;

			auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::recvBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(recv_attrs.error());

			auto prettyPrintFlags = [](tcflag_t flags,
			                           std::map<tcflag_t, std::string> map) -> std::string {
				std::string ret = "";
				tcflag_t leftover = flags;
				for (auto &[val, name] : map) {
					if (flags & val) {
						leftover &= ~val;
						ret.append(std::format("{} ", name));
					}
				}

				if (leftover)
					ret.append(std::format("0o{:o}", leftover));

				return ret;
			};

			if (logAttrs) {
				std::map<tcflag_t, std::string> iflags = {
				    {INLCR, "INLCR"},
				    {ICRNL, "ICRNL"},
				    {IXON, "IXON"},
				    {IUTF8, "IUTF8"},
				};

				std::map<tcflag_t, std::string> oflags = {
				    {OPOST, "OPOST"},
				    {ONLCR, "ONLCR"},
				};

				std::map<tcflag_t, std::string> cflags = {
				    {CREAD, "CREAD"},
				    {HUPCL, "HUPCL"},
				};

				std::map<tcflag_t, std::string> lflags = {
				    {ISIG, "ISIG"},
				    {ICANON, "ICANON"},
				    {XCASE, "XCASE"},
				    {ECHO, "ECHO"},
				    {ECHOE, "ECHOE"},
				    {ECHOK, "ECHOK"},
				    {ECHONL, "ECHONL"},
				    {ECHOCTL, "ECHOCTL"},
				    {ECHOPRT, "ECHOPRT"},
				    {ECHOKE, "ECHOKE"},
				    {NOFLSH, "NOFLSH"},
				    {TOSTOP, "TOSTOP"},
				    {PENDIN, "PENDIN"},
				    {IEXTEN, "IEXTEN"},
				};

				std::cout << "posix: TCSETS request\n"
				          << "   iflag: " << prettyPrintFlags(attrs.c_iflag, iflags) << '\n'
				          << "   oflag: " << prettyPrintFlags(attrs.c_oflag, oflags) << '\n'
				          << "   cflag: " << prettyPrintFlags(attrs.c_cflag, cflags) << '\n'
				          << "   lflag: " << prettyPrintFlags(attrs.c_lflag, lflags) << '\n';
				for (int i = 0; i < NCCS; i++) {
					std::cout << std::dec << "   cc[" << i << "]: 0x" << std::hex
					          << (int)attrs.c_cc[i];
					if (i + 1 < NCCS)
						std::cout << '\n';
				}
				std::cout << std::dec << std::endl;
			}

			ttyCopyTermios(attrs, _channel->activeSettings);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCGWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_width(_channel->width);
			resp.set_pts_height(_channel->height);
			resp.set_pts_pixel_width(_channel->pixelWidth);
			resp.set_pts_pixel_height(_channel->pixelHeight);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCSWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			if (logAttrs)
				std::cout << "posix: PTS window size is now " << req->pts_width() << "x"
				          << req->pts_height() << " chars, " << req->pts_pixel_width() << "x"
				          << req->pts_pixel_height() << " pixels (set by slave)" << std::endl;

			_channel->width = req->pts_width();
			_channel->height = req->pts_height();
			_channel->pixelWidth = req->pts_pixel_width();
			_channel->pixelHeight = req->pts_pixel_height();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			// XXX: This should deliver SIGWINCH to the parent under certain conditions
			UserSignal info;
			_channel->cts.issueSignalToForegroundGroup(SIGWINCH, info);
		} else if (req->command() == TIOCSCTTY || req->command() == TIOCGPGRP ||
		           req->command() == TIOCSPGRP || req->command() == TIOCGSID) {
			co_await _channel->commonIoctl(process, id, std::move(msg), std::move(conversation));
		} else if (req->command() == TIOCINQ) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (_channel->slaveQueue.empty()) {
				resp.set_fionread_count(0);
			} else {
				auto packet = &_channel->slaveQueue.front();
				resp.set_fionread_count(packet->buffer.size() - packet->offset);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if (req->command() == TIOCGPTN) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_index(_channel->ptsIndex);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			std::cout << "\e[31m"
			             "posix: Rejecting unknown PTS slave ioctl "
			          << req->command() << "\e[39m" << std::endl;
		}
	} else {
		std::cout << "\e[31m"
		             "posix: Rejecting unknown PTS slave ioctl message "
		          << id << "\e[39m" << std::endl;
	}
}

async::result<frg::expected<Error, std::string>> SlaveFile::ttyname() {
	std::shared_ptr<FsLink> me = associatedLink();
	std::string name;
	if (!isTerminal())
		co_return Error::notTerminal;

	name = me->getName();
	;

	// TODO: dynamically resolve absolute path?
	co_return std::string("/dev/pts/").append(name);
}

//-----------------------------------------------------------------------------
// Link and RootLink implementation.
//-----------------------------------------------------------------------------

std::shared_ptr<FsNode> Link::getOwner() { return _root->shared_from_this(); }

std::string Link::getName() { return _name; }

std::shared_ptr<FsNode> Link::getTarget() { return _device; }

RootLink::RootLink() : _root(std::make_shared<RootNode>()) {}

std::shared_ptr<FsNode> RootLink::getTarget() { return _root->shared_from_this(); }

std::shared_ptr<RootLink> globalRootLink = std::make_shared<RootLink>();

} // anonymous namespace

std::shared_ptr<UnixDevice> createMasterDevice() { return std::make_shared<MasterDevice>(); }

std::shared_ptr<FsLink> getFsRoot() { return globalRootLink; }

} // namespace pts
