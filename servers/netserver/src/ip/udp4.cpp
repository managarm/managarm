#include "udp4.hpp"

#include "ip4.hpp"
#include "checksum.hpp"

#include <async/basic.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <arch/bit.hpp>
#include <protocols/fs/server.hpp>
#include <cstring>
#include <iomanip>
#include <random>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

namespace {
struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

template<typename T>
void maybeFlip(T &x) {
	x = arch::convert_endian<arch::endian::big, arch::endian::native>(x);
}

struct PseudoHeader {
	uint32_t src;
	uint32_t dst;

	uint8_t zero = 0;
	uint8_t proto = static_cast<uint8_t>(IpProto::udp);
	uint16_t len;

	void ensureEndian() {
		maybeFlip(src);
		maybeFlip(dst);
		maybeFlip(proto);
		maybeFlip(len);
	}
};
} // namespace

struct Udp {
	struct Header {
		uint16_t src;
		uint16_t dst;
		uint16_t len;
		uint16_t chk;
	
		void ensureEndian() {
			maybeFlip(src);
			maybeFlip(dst);
			maybeFlip(len);
			maybeFlip(chk);
		}
	} header;
	static_assert(sizeof(header) == 8, "udp header size wrong");

	arch::dma_buffer_view payload() const {
		return packet->payload().subview(sizeof(header));
	}

	bool parse(smarter::shared_ptr<const Ip4Packet> packet) {
		Checksum chk;
		auto payload = packet->payload();
		if (payload.size() < sizeof(header)) {
			return false;
		}
		std::memcpy(&header, payload.data(), sizeof(header));
		header.ensureEndian();
		if (payload.size() < header.len) {
			return false;
		}
		if (header.chk != 0) {
			PseudoHeader phdr;
			phdr.src = packet->header.source;
			phdr.dst = packet->header.destination;
			phdr.proto = packet->header.protocol;
			phdr.len = payload.size();
			phdr.ensureEndian();

			chk.update(&phdr, sizeof(phdr));
			chk.update(payload);
			auto fin = chk.finalize();
			if (fin != 0 && ~fin != 0) {
				return false;
			}
		}
		this->packet = std::move(packet);
		return true;
	}

	smarter::shared_ptr<const Ip4Packet> packet;
};

Endpoint &Endpoint::operator=(struct sockaddr_in sa) {
	using arch::convert_endian;
	using arch::endian;
	port = convert_endian<endian::big, endian::native>(sa.sin_port);
	addr = convert_endian<endian::big,
	     endian::native>(sa.sin_addr.s_addr);
	return *this;
}

void Endpoint::ensureEndian() {
	maybeFlip(addr);
	maybeFlip(port);
}


bool operator<(const Endpoint &l, const Endpoint &r) {
	return std::tie(l.port, l.addr) < std::tie(r.port, r.addr);
}

namespace {
auto checkAddress(const void *addr_ptr, size_t addr_len, Endpoint &e) {
	struct sockaddr_in addr;
	if (addr_len < sizeof(addr)) {
		return protocols::fs::Error::illegalArguments;
	}
	std::memcpy(&addr, addr_ptr, sizeof(addr));
	if (addr.sin_family != AF_INET) {
		return protocols::fs::Error::afNotSupported;
	}
	e = addr;
	return protocols::fs::Error::none;
}
} // namespace

using namespace protocols::fs;

struct Udp4Socket {
	Udp4Socket(Udp4 *parent) : parent_(parent) {}

	~Udp4Socket() {
		parent_->unbind(local_);
	}

	static auto make_socket(Udp4 *parent) {
		auto s = smarter::make_shared<Udp4Socket>(parent);
		s->holder_ = s;
		return s;
	}

	static async::result<protocols::fs::Error> connect(void* obj,
			const char *creds,
			const void *addr_ptr, size_t addr_size) {
		(void) creds;

		auto self = static_cast<Udp4Socket *>(obj);
		Endpoint remote;

		if (auto e = checkAddress(addr_ptr, addr_size, remote);
			e != protocols::fs::Error::none) {
			co_return e;
		}

		if (self->local_.port == 0
				&& !self->bindAvailable()) {
			std::cout << "netserver: no source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		if (remote.addr == INADDR_BROADCAST) {
			std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		self->remote_ = remote;
		co_return protocols::fs::Error::none;
	}

	static async::result<protocols::fs::Error> bind(void* obj,
			const char *creds,
			const void *addr_ptr, size_t addr_size) {
		(void) creds;

		auto self = static_cast<Udp4Socket *>(obj);
		Endpoint local;

		if (self->local_.port != 0) {
			co_return protocols::fs::Error::illegalArguments;
		}

		if (auto e = checkAddress(addr_ptr, addr_size, local);
			e != protocols::fs::Error::none) {
			co_return e;
		}

		// TODO(arsen): check other broadcast addresses too
		if (local.addr == INADDR_BROADCAST) {
			std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		if (!ip4().hasIp(local.addr)) {
			std::cout << "netserver: not local ip" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		if (local.port == 0) {
			if (!self->bindAvailable(local.addr)) {
				co_return protocols::fs::Error::addressInUse;
			}
			std::cout << "netserver: no source port" << std::endl;
		} else if (!self->parent_->tryBind(self->holder_.lock(), local)) {
			std::cout << "netserver: address in use" << std::endl;
			co_return protocols::fs::Error::addressInUse;
		}

		co_return protocols::fs::Error::none;
	}

	static async::result<RecvResult> recvmsg(void *obj,
			const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		(void) creds;
		(void) flags;
		(void) max_ctrl_len;

		using arch::convert_endian;
		using arch::endian;
		auto self = static_cast<Udp4Socket *>(obj);
		auto element = co_await self->queue_.async_get();
		auto packet = element->payload();
		auto copy_size = std::min(packet.size(), len);
		std::memcpy(data, packet.data(), copy_size);
		sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_port = convert_endian<endian::big>(element->header.src);
		addr.sin_addr = {
			convert_endian<endian::big>(element->packet->header.source)
		};
		std::memset(addr_buf, 0, addr_size);
		std::memcpy(addr_buf, &addr, std::min(addr_size, sizeof(addr)));
		co_return RecvData{{}, copy_size, sizeof(addr), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *obj,
			const char *creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds) {
		(void) creds;
		(void) flags;
		(void) fds;

		using arch::convert_endian;
		using arch::endian;
		auto self = static_cast<Udp4Socket *>(obj);
		Endpoint target;
		auto source = self->local_;
		if (addr_size != 0) {
			if (auto e = checkAddress(addr_ptr, addr_size, target);
				e != protocols::fs::Error::none) {
				std::cout << "netserver: trimmed sendmsg addr" << std::endl;
				co_return e;
			}
		} else {
			target = self->remote_;
		}

		if (target.port == 0 || target.addr == 0) {
			std::cout << "netserver: udp needs destination" << std::endl;
			co_return protocols::fs::Error::destAddrRequired;
		}

		if (source.port == 0 && !self->bindAvailable(source.addr)) {
			std::cout << "netserver: no source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		source = self->local_;

		if (target.addr == INADDR_BROADCAST) {
			std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		std::vector<char> buf;
		buf.resize(sizeof(Udp::Header) + len);
		Udp::Header header {
			.src = source.port,
			.dst = target.port,
			.len = static_cast<uint16_t>(len + sizeof(Udp::Header)),
			.chk = 0,
		};
		header.ensureEndian();

		// native endian target IP
		auto targetIpNe = target.addr;
		source.ensureEndian();
		target.ensureEndian();

		auto ti = co_await ip4().targetByRemote(targetIpNe);
		if (!ti) {
			co_return protocols::fs::Error::netUnreachable;
		}

		Checksum chk;
		PseudoHeader psh {
			.src = convert_endian<endian::big>(ti->source),
			.dst = target.addr,
			.len = header.len
		};
		chk.update(&psh, sizeof(psh));
		chk.update(&header, sizeof(header));
		chk.update(data, len);
		header.chk = convert_endian<endian::big>(chk.finalize());

		std::cout << "netserver:" << std::endl << std::hex
			<< std::setw(8) << psh.src << std::endl
			<< std::setw(8) << psh.dst << std::endl
			<< std::setw(8) << psh.len << std::endl

			<< std::setw(8) << header.src << std::endl
			<< std::setw(8) << header.dst << std::endl
			<< std::setw(8) << header.len << std::endl
			<< std::setw(8) << header.chk << std::endl << std::dec;

		if (header.chk == 0) {
			header.chk = ~header.chk;
		}

		std::memcpy(buf.data(), &header, sizeof(header));
		std::memcpy(buf.data() + sizeof(header), data, len);

		auto error = co_await ip4().sendFrame(std::move(*ti),
			buf.data(), buf.size(),
			static_cast<uint16_t>(IpProto::udp));
		if (error != protocols::fs::Error::none) {
			co_return error;
		}
		co_return len;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *obj, uint64_t past_seq, int mask, async::cancellation_token cancellation) {
		auto self = static_cast<Udp4Socket *>(obj);
		(void)mask; // TODO: utilize mask.

		assert(past_seq <= self->_currentSeq);
		while(past_seq == self->_currentSeq && !cancellation.is_cancellation_requested())
			co_await self->_statusBell.async_wait(cancellation);

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(self->_inSeq > past_seq)
			edges |= EPOLLIN;

		co_return protocols::fs::PollWaitResult(self->_currentSeq, edges);
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *obj) {
		auto self = static_cast<Udp4Socket *>(obj);
		int events = EPOLLOUT;
		if(!self->queue_.empty())
			events |= EPOLLIN;

		co_return protocols::fs::PollStatusResult(self->_currentSeq, events);
	}

	constexpr static FileOperations ops {
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.bind = &bind,
		.connect = &connect,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
	};

	bool bindAvailable(uint32_t addr = INADDR_ANY) {
		static std::mt19937 rng;
		static std::uniform_int_distribution<uint16_t> dist {
			32768, 60999
		};
		// TODO(arsen): this rng probably is suboptimal, at some point
		// in the future replace it with a CSRNG or a hash function
		// see also: RFC6056, Section 3.3.3
		auto number = dist(rng);
		auto range_size = dist.b() - dist.a();
		auto shared_from_this = holder_.lock();
		// TODO(arsen): optimize to not call lower_bound every time?
		// I believe that such a thing is not needed right now: nearly
		// (read: absolutely) every case is is an immediate miss: we are
		// using next to nothing in this region, or any other region for
		// that manner
		for (int i = 0; i < range_size; i++) {
			uint16_t port = dist.a() + ((number + i) % range_size);
			if (parent_->tryBind(shared_from_this, { addr, port })) {
				return true;
			}
		}
		return false;
	}

private:
	friend struct Udp4;

	async::queue<Udp, stl_allocator> queue_;
	Endpoint remote_;
	Endpoint local_;
	Udp4 *parent_;
	smarter::weak_ptr<Udp4Socket> holder_;

	async::recurring_event _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;
};

void Udp4::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet) {
	Udp udp;
	if (!udp.parse(std::move(packet))) {
		std::cout << "netserver: broken udp received" << std::endl;
		return;
	}

	std::cout << "received udp datagram to port " << udp.header.dst << std::endl;

	auto i = binds.lower_bound({ 0, udp.header.dst });
	for (; i != binds.end() && i->first.port == udp.header.dst; i++) {
		auto ep = i->first;
		if (ep.addr == udp.packet->header.destination
			|| ep.addr == INADDR_ANY) {
			i->second->queue_.emplace(std::move(udp));
			i->second->_inSeq = ++i->second->_currentSeq;
			i->second->_statusBell.raise();
			break;
		}
	}
}

bool Udp4::tryBind(smarter::shared_ptr<Udp4Socket> socket, Endpoint addr) {
	auto i = binds.lower_bound(addr);
	for (; i != binds.end() && i->first.port == addr.port; i++) {
		auto ep = i->first;
		if (ep.addr == INADDR_ANY || addr.addr == INADDR_ANY
			|| ep.addr == addr.addr) {
			return false;
		}
	}
	socket->local_ = addr;
	binds.emplace(addr, std::move(socket));
	return true;
}

bool Udp4::unbind(Endpoint e) {
	return binds.erase(e) != 0;
}

void Udp4::serveSocket(helix::UniqueLane lane) {
	using protocols::fs::servePassthrough;
	auto sock = Udp4Socket::make_socket(this);
	async::detach(servePassthrough(std::move(lane), std::move(sock),
			&Udp4Socket::ops));
}
