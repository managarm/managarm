#include <arch/io_space.hpp>
#include <async/queue.hpp>
#include <frg/span.hpp>
#include <frg/vector.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/gdbserver.hpp>

namespace thor {

namespace {

enum class ProtocolError {
	none,
	unknownPacket,
	malformedPacket
};

enum class ResponseStage {
	none,
	responseReady,
	responseSent
};

void hexdump(frg::span<uint8_t> s) {
	for(size_t i = 0; i < s.size(); i += 8) {
		auto log = infoLogger();
		log << "   ";
		for(size_t j = 0; j < 8; ++j) {
			if(i + j >= s.size())
				log << "   ";
			else
				log << " " << frg::hex_fmt(s[i + j]);
		}
		log << "    |";
		for(size_t j = 0; j < 8; ++j) {
			if(i + j >= s.size())
				log << " ";
			else if(s[i + j] < 32 || s[i + j] >= 127)
				log << ".";
			else
				log << frg::char_fmt(s[i + j]);
		}
		log << "|";
		log << frg::endlog;
	}
}

// This gives a more precise assertion failure if our assumption is broken.
uint8_t unwrapByte(frg::optional<uint8_t> maybeByte) {
	assert(maybeByte); // We never cancel async_get(), hence we always get a value.
	return *maybeByte;
}

bool is_hex(uint8_t h) {
	return (h >= 'a' && h <= 'f') || (h >= '0' && h <= '9');
}

unsigned int hex2int(uint8_t h) {
	if(h >= 'a' && h <= 'f')
		return 10 + h - 'a';
	assert(h >= '0' && h <= '9');
	return h - '0';
}

uint8_t int2hex(unsigned int v) {
	if(v < 10)
		return '0' + v;
	assert(v < 16);
	return 'a' + v - 10;
}

uint8_t computeCsum(frg::span<uint8_t> s) {
	uint8_t sum = 0;
	for(size_t i = 0; i < s.size(); ++i)
		sum += s[i];
	return sum;
}

struct GdbServer {
	GdbServer(smarter::shared_ptr<Thread, ActiveHandle> thread,
		frg::string_view path, smarter::shared_ptr<WorkQueue> wq)
	: thread_{std::move(thread)}, path_{path}, wq_{std::move(wq)} { }

	coroutine<void> run();

private:
	coroutine<frg::expected<ProtocolError>> handleRequest_();

	smarter::shared_ptr<Thread, ActiveHandle> thread_;
	frg::string_view path_;
	smarter::shared_ptr<WorkQueue> wq_;

	// These are public for now, such that external code and help us send/receive data.
public:
	async::queue<uint8_t, KernelAlloc> recvQ{*kernelAlloc};
	frg::vector<uint8_t, KernelAlloc> sendQ{*kernelAlloc};

private:
	// Internal buffer for parsing / emitting packets.
	frg::vector<uint8_t, KernelAlloc> inBuffer_{*kernelAlloc};
	frg::vector<uint8_t, KernelAlloc> outBuffer_{*kernelAlloc};

	// Whether we are currently sending a response or not.
	ResponseStage responseStage_ = ResponseStage::none;
};

struct ParseView {
	ParseView() = default;

	ParseView(frg::span<uint8_t> bs)
	: bs_{bs} { }

	bool matchString(const char *s) {
		size_t n;
		for(n = 0; s[n]; ++n) {
			if(n == bs_.size())
				return false;
			if(bs_[n] != s[n])
				return false;
		}
		bs_ = bs_.subspan(n);
		return true;
	}

	bool splitDelimiter(ParseView &out, char c) {
		for(size_t n = 0; n < bs_.size(); ++n) {
			if(bs_[n] != c)
				continue;
			out = frg::span<uint8_t>{bs_.data(), n};
			bs_ = bs_.subspan(n + 1);
			return true;
		}
		infoLogger() << "splitDelimiter fail" << frg::endlog; // FIXME
		return false;
	}

	bool parseHex64(uint64_t &out) {
		// Parse the first hex char.
		if(!bs_.size())
			return false;
		if(!is_hex(bs_[0]))
			return false;
		uint64_t v = hex2int(bs_[0]);

		// Parse additional hex chars.
		size_t n;
		for(n = 1; n < bs_.size(); ++n) {
			auto c = bs_[n];
			if(is_hex(c))
				v = (v << 4) | hex2int(c);
			else
				break;
		}

		bs_ = bs_.subspan(n);
		out = v;
		return true;
	}

	bool fullyConsumed() {
		return !bs_.size();
	}

private:
	frg::span<uint8_t> bs_;
};

struct EmitOverlay {
	EmitOverlay(frg::vector<uint8_t, KernelAlloc> *buf)
	: buf_{buf} { }

	void appendString(const char *s) {
		for(size_t n = 0; s[n]; ++n)
			buf_->push_back(s[n]);
	}

	void appendHexByte(uint8_t b) {
		buf_->push_back(int2hex(b >> 4));
		buf_->push_back(int2hex(b & 0xF));
	}

	// Append a 32-bit integer in little-endian hex encoding.
	void appendLeHex32(uint32_t v) {
		for(int i = 0; i < 4; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	// Append a 64-bit integer in little-endian hex encoding.
	void appendLeHex64(uint64_t v) {
		for(int i = 0; i < 8; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	void appendBinary(frg::span<const uint8_t> b) {
		for(size_t i = 0; i < b.size(); ++i) {
			switch(b[i]) {
			case '}':
			case '$':
			case '#':
			case '*':
				buf_->push_back('}');
				buf_->push_back(b[i] ^ 0x20);
			default:
				buf_->push_back(b[i]);
			}
		}
	}

private:
	frg::vector<uint8_t, KernelAlloc> *buf_;
};

coroutine<void> GdbServer::run() {
	while(true) {
		if(responseStage_ == ResponseStage::responseReady) {
			// Send the packet.
			sendQ.push_back('$');
			for(size_t i = 0; i < outBuffer_.size(); ++i)
				sendQ.push_back(outBuffer_[i]);
			sendQ.push_back('#');

			auto csum = computeCsum({outBuffer_.data(), outBuffer_.size()});
			sendQ.push_back(int2hex(csum >> 4));
			sendQ.push_back(int2hex(csum & 0xF));
			responseStage_ = ResponseStage::responseSent;
		}

		auto firstByte = unwrapByte(co_await recvQ.async_get());

		if(firstByte == '$') {
			inBuffer_.clear();

			// Process the bytes.
			while(true) {
				auto byte = unwrapByte(co_await recvQ.async_get());
				if(byte == '#')
					break;
				inBuffer_.push_back(byte);
			}

			auto csumByte1 = unwrapByte(co_await recvQ.async_get());
			auto csumByte2 = unwrapByte(co_await recvQ.async_get());

			if(responseStage_ != ResponseStage::none) {
				infoLogger() << "thor, gdbserver: Ignoring ill-sequenced request" << frg::endlog;
				continue;
			}

			// Verify checksum.
			if(!is_hex(csumByte1) || !is_hex(csumByte2)) {
				infoLogger() << "thor, gdbserver: NACK due to missing checksum" << frg::endlog;
				sendQ.push_back('-');
				continue;
			}
			auto csum = (hex2int(csumByte1) << 4) | hex2int(csumByte2);
			auto expectedCsum = computeCsum({inBuffer_.data(), inBuffer_.size()});
			if(csum != expectedCsum) {
				infoLogger() << "thor, gdbserver: NACK due to checksum mismatch" << frg::endlog;
				sendQ.push_back('-');
				continue;
			}

			// Ack the packet.
			sendQ.push_back('+');

			auto outcome = co_await handleRequest_();
			if(!outcome) {
				if(outcome.error() == ProtocolError::unknownPacket) {
					infoLogger() << "thor, gdbserver: Unknown packet,"
							" dumping:" << frg::endlog;
				}else{
					assert(outcome.error() == ProtocolError::malformedPacket);
					infoLogger() << "thor, gdbserver: Remote violated procotol specification,"
							" dumping:" << frg::endlog;
				}
				hexdump({inBuffer_.data(), inBuffer_.size()});
			}

			responseStage_ = ResponseStage::responseReady;
		}else if(firstByte == '+') {
			if(responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::none;
			}else{
				infoLogger() << "thor, gdbserver: Ignoring stray ACK" << frg::endlog;
			}
		}else if(firstByte == '-') {
			if(responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::responseReady;
			}else{
				infoLogger() << "thor, gdbserver: Ignoring stray NACK" << frg::endlog;
			}
		}else{
			infoLogger() << "thor, gdbserver: Packet starts with unexpected byte: "
					<< frg::hex_fmt(firstByte) << frg::endlog;
		}
	}
}

coroutine<frg::expected<ProtocolError>> GdbServer::handleRequest_() {
	assert(outBuffer_.empty());
	ParseView req{{inBuffer_.data(), inBuffer_.size()}};
	EmitOverlay resp{&outBuffer_};

	if(req.matchString("H")) { // Set thread.
		// TODO: consider the argument (= thread ID).
		resp.appendString("OK");
	}else if(req.matchString("?")) { // Reason for stopping.
		if(!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		resp.appendString("S0b");
	}else if(req.matchString("g")) { // Read registers.
		if(!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		resp.appendLeHex64(thread_->_executor.general()->rax);
		resp.appendLeHex64(thread_->_executor.general()->rbx);
		resp.appendLeHex64(thread_->_executor.general()->rcx);
		resp.appendLeHex64(thread_->_executor.general()->rdx);
		resp.appendLeHex64(thread_->_executor.general()->rsi);
		resp.appendLeHex64(thread_->_executor.general()->rdi);
		resp.appendLeHex64(thread_->_executor.general()->rbp);
		resp.appendLeHex64(thread_->_executor.general()->rsp);
		resp.appendLeHex64(thread_->_executor.general()->r8);
		resp.appendLeHex64(thread_->_executor.general()->r9);
		resp.appendLeHex64(thread_->_executor.general()->r10);
		resp.appendLeHex64(thread_->_executor.general()->r11);
		resp.appendLeHex64(thread_->_executor.general()->r12);
		resp.appendLeHex64(thread_->_executor.general()->r13);
		resp.appendLeHex64(thread_->_executor.general()->r14);
		resp.appendLeHex64(thread_->_executor.general()->r15);
		resp.appendLeHex64(thread_->_executor.general()->rip);
		resp.appendLeHex32(thread_->_executor.general()->rflags);
		resp.appendLeHex32(thread_->_executor.general()->cs);
		resp.appendLeHex32(thread_->_executor.general()->ss);
		resp.appendLeHex32(thread_->_executor.general()->ss); // DS
		resp.appendLeHex32(thread_->_executor.general()->ss); // ES
		resp.appendLeHex32(thread_->_executor.general()->clientFs);
		resp.appendLeHex32(thread_->_executor.general()->clientGs);
		for(int i = 0; i < 8; ++i) // 8 FPU registers.
			for(int j = 0; j < 10; ++j) // 80 bits in size.
				resp.appendString("xx");
		for(int i = 0; i < 8; ++i) // 8 FPU control registers.
			for(int j = 0; j < 4; ++j)
				resp.appendString("xx");
	}else if(req.matchString("m")) { // Read memory.
		uint64_t address;
		uint64_t length;
		if(!req.parseHex64(address)
				|| !req.matchString(",")
				|| !req.parseHex64(length)
				|| !req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		frg::vector<uint8_t, KernelAlloc> mem{*kernelAlloc};
		mem.resize(length);
		{
			auto lockHandle = AddressSpaceLockHandle{thread_->getAddressSpace().lock(),
					reinterpret_cast<void *>(address), length};
			co_await lockHandle.acquire(wq_);
			lockHandle.load(0, mem.data(), length);
		}

		for(size_t i = 0; i < length; ++i)
			resp.appendHexByte(mem[i]);
	}else if(req.matchString("q")) { // General query.
		if(req.matchString("Supported")) {
			resp.appendString("qXfer:exec-file:read+;");
		}else if(req.matchString("Xfer")) {
			ParseView object, annex;
			uint64_t offset, length;
			if(!req.matchString(":")
					|| !req.splitDelimiter(object, ':')
					|| !req.matchString("read:") // TODO: Support writes.
					|| !req.splitDelimiter(annex, ':')
					|| !req.parseHex64(offset)
					|| !req.matchString(",")
					|| !req.parseHex64(length))
				co_return ProtocolError::malformedPacket;

			frg::optional<frg::span<const uint8_t>> s;

			if(object.matchString("exec-file") && object.fullyConsumed()) {
				// TODO: consider the annex (= process ID).
				s = frg::span<const uint8_t>{reinterpret_cast<const uint8_t *>(path_.data()),
						path_.size()};
			}

			if(s) {
				if(offset >= s->size()) {
					// End-of-object (offset beyond object size).
					resp.appendString("l");
				}else if(offset + length >= s->size()) {
					// End-of-object.
					resp.appendString("l");
					resp.appendBinary(s->subspan(offset));
				}else{
					// More data available.
					resp.appendString("m");
					resp.appendBinary(s->subspan(offset, length));
				}
			}
		}else{
			co_return ProtocolError::unknownPacket;
		}
	}else if(req.matchString("v")) { // Multi-letter requests.
		if(req.matchString("MustReplyEmpty")) {
			// Must be handled like unknown v packets (but do not complain).
			req = {}; // Fully consume the packet.
		}else{
			co_return ProtocolError::unknownPacket;
		}
	}else{
		co_return ProtocolError::unknownPacket;
	}

	co_return {};
}

inline constexpr arch::scalar_register<uint8_t> uartData(0);
inline constexpr arch::bit_register<uint8_t> lineStatus(5);

inline constexpr arch::field<uint8_t, bool> rxReady(0, 1);
inline constexpr arch::field<uint8_t, bool> txReady(5, 1);

} // anonymous namespace

void launchGdbServer(smarter::shared_ptr<Thread, ActiveHandle> thread,
		frg::string_view path, smarter::shared_ptr<WorkQueue> wq) {
#ifdef __x86_64__
	infoLogger() << "thor: Launching gdbserver" << frg::endlog;

	auto svr = frg::construct<GdbServer>(*kernelAlloc,
			std::move(thread), path, std::move(wq));

	// Start the actual GDB server.
	async::detach_with_allocator(*kernelAlloc, svr->run());

	// Start the byte stream transport.
	// TODO: Generalize this to other (and better!) transport mechanisms.
	async::detach_with_allocator(*kernelAlloc, [] (GdbServer *svr) -> coroutine<void> {
		auto uartBase = arch::global_io.subspace(0x3F8);
		while(true) {
			// Ready all data that is available.
			while(uartBase.load(lineStatus) & rxReady) {
				uint8_t b = uartBase.load(uartData);
				svr->recvQ.put(b);
			}

			// Transmit all buffered data.
			if(!svr->sendQ.empty()) {
				for(size_t i = 0; i < svr->sendQ.size(); ++i) {
					while(!(uartBase.load(lineStatus) & txReady))
						;
					uartBase.store(uartData, svr->sendQ[i]);
				}
				svr->sendQ.clear();
			}

			// 10ms is quite expensive, OTOH we want to switch to IRQs anyway.
			auto pre = systemClockSource()->currentNanos();
			co_await generalTimerEngine()->sleepFor(10'000'000);
			auto elapsed = systemClockSource()->currentNanos() - pre;
			if(elapsed > 100'000'000)
				infoLogger() << "elapsed: " << elapsed << frg::endlog;
		}
	}(svr));
#else
	infoLogger() << "thor: No suitable transport for gdbserver" << frg::endlog;
#endif
}

} // namespace thor
