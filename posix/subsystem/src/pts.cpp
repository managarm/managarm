#include <asm/ioctls.h>
#include <termios.h>
#include <sys/epoll.h>
#include <sstream>

#include <async/recurring-event.hpp>

#include "file.hpp"
#include "process.hpp"
#include "pts.hpp"
#include "fs.bragi.hpp"

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
	Channel(int pts_index)
	: ptsIndex{pts_index}, currentSeq{1}, masterInSeq{0}, slaveInSeq{0} {
		auto ctrl = [] (char c) -> char { // Convert ^X to X.
			return c - 64;
		};

		memset(&activeSettings, 0, sizeof(struct termios));
		// cflag: Linux also stores a baud rate here.
		// lflag: Linux additionally sets ECHOCTL, ECHOKE (which we do not have).
		activeSettings.c_iflag = ICRNL | IXON;
		activeSettings.c_oflag = OPOST | ONLCR;
		activeSettings.c_cflag = CS8 | CREAD | HUPCL;
		activeSettings.c_lflag = ECHO | ECHOE | ECHOK | ISIG | ICANON | IEXTEN;
		activeSettings.c_cc[VINTR] = ctrl('C');
		activeSettings.c_cc[VEOF] = ctrl('D');
		activeSettings.c_cc[VKILL] = ctrl('U');
		activeSettings.c_cc[VSTART] = ctrl('Q');
		activeSettings.c_cc[VSTOP] = ctrl('S');
		activeSettings.c_cc[VSUSP] = ctrl('Z');
		activeSettings.c_cc[VQUIT] = ctrl('\\');
		activeSettings.c_cc[VERASE] = 127; // DEL character.
		activeSettings.c_cc[VMIN] = 1;
	}

	async::result<void> commonIoctl(Process *process, managarm::fs::CntRequest req, helix::UniqueLane conversation);

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

//-----------------------------------------------------------------------------
// Device and file structs.
//-----------------------------------------------------------------------------

struct MasterDevice final : UnixDevice {
	MasterDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({5, 2});
	}

	std::string nodePath() override {
		return "ptmx";
	}

	async::result<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
};

struct SlaveDevice final : UnixDevice {
	SlaveDevice(std::shared_ptr<Channel> channel);

	std::string nodePath() override {
		return std::string{};
	}

	async::result<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;

private:
	std::shared_ptr<Channel> _channel;
};

struct MasterFile final : File {
public:
	static void serve(smarter::shared_ptr<MasterFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations));
	}

	MasterFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			bool nonBlocking);

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t maxLength) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<void>
	ioctl(Process *process, managarm::fs::CntRequest req, helix::UniqueLane conversation) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;

	bool _nonBlocking;
};

struct SlaveFile final : File {
public:
	static void serve(smarter::shared_ptr<SlaveFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations));
	}

	SlaveFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			std::shared_ptr<Channel> channel);

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t maxLength) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<void>
	ioctl(Process *process, managarm::fs::CntRequest req, helix::UniqueLane conversation) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;
};

//-----------------------------------------------------------------------------
// File system structs.
//-----------------------------------------------------------------------------

struct Link final : FsLink {
public:
	explicit Link(RootNode *root, std::string name, std::shared_ptr<DeviceNode> device)
	: _root{root}, _name{std::move(name)}, _device{std::move(device)} { }

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

	RootNode *rootNode() {
		return _root.get();
	}

	std::shared_ptr<FsNode> getOwner() override {
		throw std::logic_error("posix: pts RootLink has no owner");
	}

	std::string getName() override {
		throw std::logic_error("posix: pts RootLink has no name");
	}

	std::shared_ptr<FsNode> getTarget() override;

private:
	std::shared_ptr<RootNode> _root;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
		return name < link->getName();
	}

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
		return a->getName() < b->getName();
	}
};

struct DeviceNode final : FsNode {
public:
	DeviceNode(DeviceId id)
	: _type{VfsType::charDevice}, _id{id} { }

	VfsType getType() override {
		return _type;
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts DeviceNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}

	DeviceId readDevice() override {
		return _id;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
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
	VfsType getType() override {
		return VfsType::directory;
	}

	void linkDevice(std::string name, std::shared_ptr<DeviceNode> node) {
		auto link = std::make_shared<Link>(this, name, std::move(node));
		_entries.insert(std::move(link));
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts RootNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
	getLink(std::string name) override {
		auto it = _entries.find(name);
		if(it != _entries.end())
			co_return *it;
		co_return nullptr; // TODO: Return an error code.
	}

private:
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

async::result<void>
Channel::commonIoctl(Process *process, managarm::fs::CntRequest req, helix::UniqueLane conversation) {
	switch(req.command()) {
	case TIOCSCTTY: {
		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto process = findProcessWithCredentials(extractCreds.credentials());

		managarm::fs::SvrResponse resp;
		if(auto e = cts.assignSessionOf(process.get()); e == Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}else if(e == Error::insufficientPermissions) {
			resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
		}else{
			assert(e == Error::success);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [sendResp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(sendResp.error());
		break;
	}
	case TIOCGPGRP: {
		managarm::fs::SvrResponse resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto process = findProcessWithCredentials(extractCreds.credentials());

		if(&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
		} else {
			resp.set_pid(cts.getSession()->getForegroundGroup()->getHull()->getPid());
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCSPGRP: {
		managarm::fs::SvrResponse resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto process = findProcessWithCredentials(extractCreds.credentials());
		auto group = process->pgPointer()->getSession()->getProcessGroupById(req.pgid());
		if(!group) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			Error ret = cts.getSession()->setForegroundGroup(group.get());
			if(ret == Error::insufficientPermissions) {
				resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
			} else {
				assert(ret == Error::success);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCGSID: {
		managarm::fs::SvrResponse resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto process = findProcessWithCredentials(extractCreds.credentials());

		if(&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
		} else {
			resp.set_pid(cts.getSession()->getSessionId());
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	default:
		std::cout << "\e[31m" "posix: Rejecting unknown PTS ioctl (commonIoctl) " << req.command()
				<< "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// MasterDevice implementation.
//-----------------------------------------------------------------------------

FutureMaybe<SharedFilePtr>
MasterDevice::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	assert(!(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)));
	auto file = smarter::make_shared<MasterFile>(std::move(mount), std::move(link),
			semantic_flags & semanticNonBlock);
	file->setupWeakFile(file);
	MasterFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

MasterFile::MasterFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		bool nonBlocking)
: File{StructName::get("pts.master"), std::move(mount), std::move(link), File::defaultPipeLikeSeek},
		_channel{std::make_shared<Channel>(nextPtsIndex++)}, _nonBlocking{nonBlocking} {
	auto slave_device = std::make_shared<SlaveDevice>(_channel);
	charRegistry.install(std::move(slave_device));

	globalRootLink->rootNode()->linkDevice(std::to_string(_channel->ptsIndex),
			std::make_shared<DeviceNode>(DeviceId{136, _channel->ptsIndex}));
}

async::result<frg::expected<Error, size_t>>
MasterFile::readSome(Process *, void *data, size_t maxLength) {
	if(logReadWrite)
		std::cout << "posix: Read from tty " << structName() << std::endl;
	if(!maxLength)
		co_return 0;

	if (_channel->masterQueue.empty() && _nonBlocking)
		co_return Error::wouldBlock;

	while(_channel->masterQueue.empty())
		co_await _channel->statusBell.async_wait();

	auto packet = &_channel->masterQueue.front();
	size_t chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	assert(chunk); // Otherwise, we return above due to !maxLength.
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if(packet->offset == packet->buffer.size())
		_channel->masterQueue.pop_front();
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MasterFile::writeAll(Process *, const void *data, size_t length) {
	if(logReadWrite)
		std::cout << "posix: Write to tty " << structName() << std::endl;

	Packet packet;
	packet.buffer.reserve(length);
	packet.offset = 0;

	auto s = reinterpret_cast<const char *>(data);
	for(size_t i = 0; i < length; i++) {
		if(_channel->activeSettings.c_lflag & ISIG) {
			if(s[i] == static_cast<char>(_channel->activeSettings.c_cc[VINTR])) {
				UserSignal info;
				_channel->cts.issueSignalToForegroundGroup(SIGINT, info);
				continue;
			}
		}

		// Not a special character. Emit to the slave.
		packet.buffer.push_back(s[i]);
	}

	// Check whether all data was discarded above.
	if(packet.buffer.size()) {
		_channel->slaveQueue.push_back(std::move(packet));
		_channel->slaveInSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
	}
	co_return length;
}

async::result<frg::expected<Error, PollWaitResult>>
MasterFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	(void)mask; // TODO: utilize mask.
	assert(past_seq <= _channel->currentSeq);

	while(past_seq == _channel->currentSeq
			&& !cancellation.is_cancellation_requested())
		co_await _channel->statusBell.async_wait(cancellation);

	// For now making pts files always writable is sufficient.
	int edges = EPOLLOUT;
	if(_channel->masterInSeq > past_seq)
		edges |= EPOLLIN;

	co_return PollWaitResult{_channel->currentSeq, edges};
}

async::result<frg::expected<Error, PollStatusResult>>
MasterFile::pollStatus(Process *) {
	// For now making pts files always writable is sufficient.
	int events = EPOLLOUT;
	if(!_channel->masterQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

async::result<void> MasterFile::ioctl(Process *process, managarm::fs::CntRequest req,
		helix::UniqueLane conversation) {
	switch(req.command()) {
	case TIOCGPTN: {
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_pts_index(_channel->ptsIndex);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCSWINSZ: {
		managarm::fs::SvrResponse resp;

		if(logAttrs)
			std::cout << "posix: PTS window size is now "
					<< req.pts_width() << "x" << req.pts_height()
					<< " chars, "
					<< req.pts_pixel_width() << "x" << req.pts_pixel_height()
					<< " pixels (set by master)" << std::endl;

		_channel->width = req.pts_width();
		_channel->height = req.pts_height();
		_channel->pixelWidth = req.pts_pixel_width();
		_channel->pixelHeight = req.pts_pixel_height();

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCSCTTY:
	case TIOCGPGRP:
	case TIOCSPGRP:
	case TIOCGSID: {
		co_await _channel->commonIoctl(process, std::move(req), std::move(conversation));
		break;
	}
	default:
		std::cout << "\e[31m" "posix: Rejecting unknown PTS master ioctl " << req.command()
				<< "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// SlaveDevice implementation.
//-----------------------------------------------------------------------------

SlaveDevice::SlaveDevice(std::shared_ptr<Channel> channel)
: UnixDevice(VfsType::charDevice), _channel{std::move(channel)} {
	assignId({136, _channel->ptsIndex});
}

FutureMaybe<SharedFilePtr>
SlaveDevice::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	assert(!(semantic_flags & ~(semanticRead | semanticWrite)));
	auto file = smarter::make_shared<SlaveFile>(std::move(mount), std::move(link), _channel);
	file->setupWeakFile(file);
	SlaveFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

SlaveFile::SlaveFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		std::shared_ptr<Channel> channel)
: File{StructName::get("pts.slave"), std::move(mount), std::move(link),
		File::defaultIsTerminal | File::defaultPipeLikeSeek},
		_channel{std::move(channel)} { }

async::result<frg::expected<Error, size_t>>
SlaveFile::readSome(Process *, void *data, size_t maxLength) {
	if(logReadWrite)
		std::cout << "posix: Read from tty " << structName() << std::endl;
	if(!maxLength)
		co_return 0;

	while(_channel->slaveQueue.empty())
		co_await _channel->statusBell.async_wait();

	auto packet = &_channel->slaveQueue.front();
	auto chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	assert(chunk); // Otherwise, we return above due to !maxLength.
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if(packet->offset == packet->buffer.size())
		_channel->slaveQueue.pop_front();
	co_return chunk;
}


async::result<frg::expected<Error, size_t>>
SlaveFile::writeAll(Process *, const void *data, size_t length) {
	if(logReadWrite)
		std::cout << "posix: Write to tty " << structName() << std::endl;

	if(!length)
		co_return {};

	// Perform output processing.
	std::stringstream ss;
	for(size_t i = 0; i < length; i++) {
		char c;
		memcpy(&c, reinterpret_cast<const char *>(data) + i, 1);
		if((_channel->activeSettings.c_oflag & ONLCR) && c == '\n') {
//			std::cout << "Mapping NL -> CR,NL" << std::endl;
			ss << "\r\n";
		}else{
//			std::cout << "c: " << (int)c << std::endl;
			ss << c;
		}
	}

	// TODO: This is very inefficient.
	auto str = ss.str();

	Packet packet;
	packet.buffer.resize(str.size());
	memcpy(packet.buffer.data(), str.data(), str.size());
	packet.offset = 0;

	_channel->masterQueue.push_back(std::move(packet));
	_channel->masterInSeq = ++_channel->currentSeq;
	_channel->statusBell.raise();
	co_return length;
}

async::result<frg::expected<Error, PollWaitResult>>
SlaveFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	(void)mask; // TODO: utilize mask.
	assert(past_seq <= _channel->currentSeq);
	while(past_seq == _channel->currentSeq
			&& !cancellation.is_cancellation_requested())
		co_await _channel->statusBell.async_wait(cancellation);

	// For now making pts files always writable is sufficient.
	int edges = EPOLLOUT;
	if(_channel->slaveInSeq > past_seq)
		edges |= EPOLLIN;

	co_return PollWaitResult{_channel->currentSeq, edges};
}

async::result<frg::expected<Error, PollStatusResult>>
SlaveFile::pollStatus(Process *) {
	// For now making pts files always writable is sufficient.
	int events = EPOLLOUT;
	if(!_channel->slaveQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

async::result<void> SlaveFile::ioctl(Process *process, managarm::fs::CntRequest req,
		helix::UniqueLane conversation) {
	switch(req.command()) {
	case TCGETS: {
		managarm::fs::SvrResponse resp;
		struct termios attrs;

		// Element-wise copy to avoid information leaks in padding.
		memset(&attrs, 0, sizeof(struct termios));
		attrs.c_iflag = _channel->activeSettings.c_iflag;
		attrs.c_oflag = _channel->activeSettings.c_oflag;
		attrs.c_cflag = _channel->activeSettings.c_cflag;
		attrs.c_lflag = _channel->activeSettings.c_lflag;
		for(int i = 0; i < NCCS; i++)
			attrs.c_cc[i] = _channel->activeSettings.c_cc[i];

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(&attrs, sizeof(struct termios))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_attrs.error());
		break;
	}
	case TCSETS: {
		struct termios attrs;
		managarm::fs::SvrResponse resp;

		auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&attrs, sizeof(struct termios))
		);
		HEL_CHECK(recv_attrs.error());

		if(logAttrs) {
			std::cout << "posix: TCSETS request\n"
					<< "    iflag: 0x" << attrs.c_iflag << '\n'
					<< "    oflag: 0x" << attrs.c_oflag << '\n'
					<< "    cflag: 0x" << attrs.c_cflag << '\n'
					<< "    lflag: 0x" << attrs.c_lflag << '\n';
			for(int i = 0; i < NCCS; i++) {
				std::cout << std::dec << "   cc[" << i << "]: 0x"
						<< std::hex << (int)attrs.c_cc[i];
				if(i + 1 < NCCS)
					std::cout << '\n';
			}
			std::cout << std::dec << std::endl;
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCGWINSZ: {
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_pts_width(_channel->width);
		resp.set_pts_height(_channel->height);
		resp.set_pts_pixel_width(_channel->pixelWidth);
		resp.set_pts_pixel_height(_channel->pixelHeight);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCSWINSZ: {
		managarm::fs::SvrResponse resp;

		if(logAttrs)
			std::cout << "posix: PTS window size is now "
					<< req.pts_width() << "x" << req.pts_height()
					<< " chars, "
					<< req.pts_pixel_width() << "x" << req.pts_pixel_height()
					<< " pixels (set by slave)" << std::endl;

		_channel->width = req.pts_width();
		_channel->height = req.pts_height();
		_channel->pixelWidth = req.pts_pixel_width();
		_channel->pixelHeight = req.pts_pixel_height();

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		break;
	}
	case TIOCSCTTY:
	case TIOCGPGRP:
	case TIOCSPGRP:
	case TIOCGSID: {
		co_await _channel->commonIoctl(process, std::move(req), std::move(conversation));
		break;
	}
	default:
		std::cout << "\e[31m" "posix: Rejecting unknown PTS slave ioctl " << req.command()
				<< "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// Link and RootLink implementation.
//-----------------------------------------------------------------------------

std::shared_ptr<FsNode> Link::getOwner() {
	return _root->shared_from_this();
}

std::string Link::getName() {
	return _name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return _device;
}

RootLink::RootLink()
: _root(std::make_shared<RootNode>()) { }

std::shared_ptr<FsNode> RootLink::getTarget() {
	return _root->shared_from_this();
}

std::shared_ptr<RootLink> globalRootLink = std::make_shared<RootLink>();

} // anonymous namespace

std::shared_ptr<UnixDevice> createMasterDevice() {
	return std::make_shared<MasterDevice>();
}

std::shared_ptr<FsLink> getFsRoot() {
	return globalRootLink;
}

} // namespace pts
