#include <frg/span.hpp>

#include "gdbserver.hpp"

namespace {

enum class ProtocolError { none, unknownPacket, malformedPacket };

enum class ResponseStage { none, responseReady, responseSent };

void hexdump(frg::span<uint8_t> s) {
	std::cout << std::hex;
	for (size_t i = 0; i < s.size(); i += 8) {
		std::cout << "   ";
		for (size_t j = 0; j < 8; ++j) {
			if (i + j >= s.size())
				std::cout << "   ";
			else
				std::cout << " " << static_cast<unsigned int>(s[i + j]);
		}
		std::cout << "    |";
		for (size_t j = 0; j < 8; ++j) {
			if (i + j >= s.size())
				std::cout << " ";
			else if (s[i + j] < 32 || s[i + j] >= 127)
				std::cout << ".";
			else
				std::cout << static_cast<char>(s[i + j]);
		}
		std::cout << "|";
		std::cout << std::endl;
	}
	std::cout << std::dec;
}

bool is_hex(uint8_t h) { return (h >= 'a' && h <= 'f') || (h >= '0' && h <= '9'); }

unsigned int hex2int(uint8_t h) {
	if (h >= 'a' && h <= 'f')
		return 10 + h - 'a';
	assert(h >= '0' && h <= '9');
	return h - '0';
}

uint8_t int2hex(unsigned int v) {
	if (v < 10)
		return '0' + v;
	assert(v < 16);
	return 'a' + v - 10;
}

uint8_t computeCsum(frg::span<uint8_t> s) {
	uint8_t sum = 0;
	for (size_t i = 0; i < s.size(); ++i)
		sum += s[i];
	return sum;
}

struct GdbServer {
	GdbServer(Process *process, smarter::shared_ptr<File, FileHandle> file)
	    : process_{std::move(process)},
	      file_{std::move(file)} {
		path_ = process_->path();
	}

	async::result<void> run();

  private:
	async::result<frg::expected<ProtocolError>> handleRequest_();

	async::result<uint8_t> recvByte_() {
		if (recvPtr_ == recvLimit_) {
			auto sizeOrError = co_await file_->readSome(nullptr, recvBuffer_, 512);
			assert(sizeOrError);
			recvLimit_ = sizeOrError.value();
			recvPtr_ = 0;
		}

		co_return recvBuffer_[recvPtr_++];
	}

	async::result<void> sendByte(uint8_t b) {
		auto sizeOrError = co_await file_->writeAll(nullptr, &b, 1);
		assert(sizeOrError);
	}

	template <size_t N> async::result<void> sendBytes(std::array<uint8_t, N> s) {
		auto sizeOrError = co_await file_->writeAll(nullptr, s.data(), s.size());
		assert(sizeOrError);
	}

	async::result<void> sendSpan(frg::span<uint8_t> s) {
		auto sizeOrError = co_await file_->writeAll(nullptr, s.data(), s.size());
		assert(sizeOrError);
	}

	Process *process_;
	std::string path_;

	smarter::shared_ptr<File, FileHandle> file_;
	uint8_t recvBuffer_[512];
	size_t recvLimit_ = 0;
	size_t recvPtr_ = 0;

	// Internal buffer for parsing / emitting packets.
	std::vector<uint8_t> inBuffer_;
	std::vector<uint8_t> outBuffer_;

	// Whether we are currently sending a response or not.
	ResponseStage responseStage_ = ResponseStage::none;
};

struct ParseView {
	ParseView() = default;

	ParseView(frg::span<uint8_t> bs) : bs_{bs} {}

	bool matchString(const char *s) {
		size_t n;
		for (n = 0; s[n]; ++n) {
			if (n == bs_.size())
				return false;
			if (bs_[n] != s[n])
				return false;
		}
		bs_ = bs_.subspan(n);
		return true;
	}

	bool matchFullString(const char *s) { return matchString(s) && fullyConsumed(); }

	bool splitDelimiter(ParseView &out, char c) {
		for (size_t n = 0; n < bs_.size(); ++n) {
			if (bs_[n] != c)
				continue;
			out = frg::span<uint8_t>{bs_.data(), n};
			bs_ = bs_.subspan(n + 1);
			return true;
		}
		return false;
	}

	bool parseHex64(uint64_t &out) {
		// Parse the first hex char.
		if (!bs_.size())
			return false;
		if (!is_hex(bs_[0]))
			return false;
		uint64_t v = hex2int(bs_[0]);

		// Parse additional hex chars.
		size_t n;
		for (n = 1; n < bs_.size(); ++n) {
			auto c = bs_[n];
			if (is_hex(c))
				v = (v << 4) | hex2int(c);
			else
				break;
		}

		bs_ = bs_.subspan(n);
		out = v;
		return true;
	}

	bool fullyConsumed() { return !bs_.size(); }

  private:
	frg::span<uint8_t> bs_;
};

struct EmitOverlay {
	EmitOverlay(std::vector<uint8_t> *buf) : buf_{buf} {}

	void appendString(const char *s) {
		for (size_t n = 0; s[n]; ++n)
			buf_->push_back(s[n]);
	}

	void appendHexByte(uint8_t b) {
		buf_->push_back(int2hex(b >> 4));
		buf_->push_back(int2hex(b & 0xF));
	}

	// Append a 32-bit integer in little-endian hex encoding.
	void appendLeHex32(uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	// Append a 64-bit integer in little-endian hex encoding.
	void appendLeHex64(uint64_t v) {
		for (int i = 0; i < 8; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	void appendBinary(frg::span<const std::byte> s) {
		for (size_t i = 0; i < s.size(); ++i) {
			auto b = static_cast<uint8_t>(s[i]);
			switch (b) {
			case '}':
			case '$':
			case '#':
			case '*':
				buf_->push_back('}');
				buf_->push_back(b ^ 0x20);
				break;
			default:
				buf_->push_back(b);
			}
		}
	}

  private:
	std::vector<uint8_t> *buf_;
};

async::result<void> GdbServer::run() {
	while (true) {
		if (responseStage_ == ResponseStage::responseReady) {
			// Send the packet.
			auto csum = computeCsum({outBuffer_.data(), outBuffer_.size()});
			co_await sendByte('$');
			co_await sendSpan({outBuffer_.data(), outBuffer_.size()});
			co_await sendBytes<3>({'#', int2hex(csum >> 4), int2hex(csum & 0xF)});
			responseStage_ = ResponseStage::responseSent;
		}

		auto firstByte = co_await recvByte_();

		if (firstByte == '$') {
			inBuffer_.clear();

			// Process the bytes.
			while (true) {
				auto byte = co_await recvByte_();
				if (byte == '#')
					break;
				inBuffer_.push_back(byte);
			}

			auto csumByte1 = co_await recvByte_();
			auto csumByte2 = co_await recvByte_();

			if (responseStage_ != ResponseStage::none) {
				std::cout << "posix, gdbserver: Ignoring ill-sequenced request" << std::endl;
				continue;
			}

			// Verify checksum.
			if (!is_hex(csumByte1) || !is_hex(csumByte2)) {
				std::cout << "posix, gdbserver: NACK due to missing checksum" << std::endl;
				co_await sendByte('-');
				continue;
			}
			auto csum = (hex2int(csumByte1) << 4) | hex2int(csumByte2);
			auto expectedCsum = computeCsum({inBuffer_.data(), inBuffer_.size()});
			if (csum != expectedCsum) {
				std::cout << "posix, gdbserver: NACK due to checksum mismatch" << std::endl;
				co_await sendByte('-');
				continue;
			}

			// Ack the packet.
			co_await sendByte('+');

			auto outcome = co_await handleRequest_();
			if (!outcome) {
				if (outcome.error() == ProtocolError::unknownPacket) {
					std::cout << "posix, gdbserver: Unknown packet,"
					             " dumping:"
					          << std::endl;
				} else {
					assert(outcome.error() == ProtocolError::malformedPacket);
					std::cout << "posix, gdbserver: Remote violated procotol specification,"
					             " dumping:"
					          << std::endl;
				}
				hexdump({inBuffer_.data(), inBuffer_.size()});
			}

			responseStage_ = ResponseStage::responseReady;
		} else if (firstByte == '+') {
			if (responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::none;
			} else {
				std::cout << "posix, gdbserver: Ignoring stray ACK" << std::endl;
			}
		} else if (firstByte == '-') {
			if (responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::responseReady;
			} else {
				std::cout << "posix, gdbserver: Ignoring stray NACK" << std::endl;
			}
		} else {
			std::cout << "posix, gdbserver: Packet starts with unexpected byte: " << std::hex
			          << firstByte << std::dec << std::endl;
		}
	}
}

async::result<frg::expected<ProtocolError>> GdbServer::handleRequest_() {
	assert(outBuffer_.empty());
	ParseView req{{inBuffer_.data(), inBuffer_.size()}};
	EmitOverlay resp{&outBuffer_};

	if (req.matchString("H")) { // Set thread.
		// TODO: consider the argument (= thread ID).
		resp.appendString("OK");
	} else if (req.matchString("?")) { // Reason for stopping.
		if (!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		resp.appendString("S0b");
	} else if (req.matchString("g")) { // Read registers.
		if (!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		uintptr_t pcrs[2];
		uintptr_t gprs[kHelNumGprs];

		HEL_CHECK(helLoadRegisters(process_->threadDescriptor().getHandle(), kHelRegsProgram, pcrs)
		);
		HEL_CHECK(helLoadRegisters(process_->threadDescriptor().getHandle(), kHelRegsGeneral, gprs)
		);

#if defined(__x86_64__)
		resp.appendLeHex64(gprs[0]);  // RAX.
		resp.appendLeHex64(gprs[1]);  // RBX.
		resp.appendLeHex64(gprs[2]);  // RCX.
		resp.appendLeHex64(gprs[3]);  // RDX.
		resp.appendLeHex64(gprs[5]);  // RSI.
		resp.appendLeHex64(gprs[4]);  // RDI.
		resp.appendLeHex64(gprs[14]); // RBP.
		resp.appendLeHex64(pcrs[1]);  // RSP.
		resp.appendLeHex64(gprs[6]);  // R8
		resp.appendLeHex64(gprs[7]);  // R9
		resp.appendLeHex64(gprs[8]);  // R10
		resp.appendLeHex64(gprs[9]);  // R11
		resp.appendLeHex64(gprs[10]); // R12
		resp.appendLeHex64(gprs[11]); // R13
		resp.appendLeHex64(gprs[12]); // R14
		resp.appendLeHex64(gprs[13]); // R15
		resp.appendLeHex64(pcrs[0]);  // RIP.
		for (int j = 0; j < 4; ++j)   // Rflags.
			resp.appendString("xx");
		for (int i = 0; i < 6; ++i) // CS, SS, DS, ES, FS, GS.
			for (int j = 0; j < 4; ++j)
				resp.appendString("xx");
		for (int i = 0; i < 8; ++i)      // 8 FPU registers.
			for (int j = 0; j < 10; ++j) // 80 bits in size.
				resp.appendString("xx");
		for (int i = 0; i < 8; ++i) // 8 FPU control registers.
			for (int j = 0; j < 4; ++j)
				resp.appendString("xx");
#else
		std::cout << "posix, gdbserver: Register access is not implemented for this architecture"
		          << std::endl;
#endif
	} else if (req.matchString("m")) { // Read memory.
		uint64_t address;
		uint64_t length;
		if (!req.parseHex64(address) || !req.matchString(",") || !req.parseHex64(length) ||
		    !req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		std::vector<uint8_t> mem;
		mem.resize(length);

		for (size_t i = 0; i < length; ++i) {
			// We load the memory byte for byte until we fail, readMemory does not support partial
			// reads yet.
			auto loadMemory = co_await helix_ng::readMemory(
			    process_->vmContext()->getSpace(), address + i, 1, mem.data() + i
			);
			if (loadMemory.error())
				break;
			resp.appendHexByte(mem[i]);
		}
	} else if (req.matchString("q")) { // General query.
		if (req.matchString("Supported")) {
			resp.appendString("qXfer:auxv:read+;");
			resp.appendString("qXfer:exec-file:read+;");
			resp.appendString("qXfer:features:read+;");
		} else if (req.matchString("Xfer")) {
			ParseView object, annex;
			uint64_t offset, length;
			if (!req.matchString(":") || !req.splitDelimiter(object, ':') ||
			    !req.matchString("read:") // TODO: Support writes.
			    || !req.splitDelimiter(annex, ':') || !req.parseHex64(offset) ||
			    !req.matchString(",") || !req.parseHex64(length))
				co_return ProtocolError::malformedPacket;

			std::optional<frg::span<const std::byte>> s;

			// If we have to dynamically generate data, we use a buffer and make s point to it.
			std::vector<std::byte> buffer;

			if (object.matchFullString("auxv") && annex.fullyConsumed()) {
				auto begin = reinterpret_cast<std::byte *>(process_->clientAuxBegin());
				auto end = reinterpret_cast<std::byte *>(process_->clientAuxEnd());
				for (auto it = begin; it != end; ++it) {
					// We load the memory byte for byte until we fail,
					// readMemory does not support partial reads yet.
					std::byte b;
					auto loadMemory = co_await helix_ng::readMemory(
					    process_->vmContext()->getSpace(), reinterpret_cast<uintptr_t>(it), 1, &b
					);
					if (loadMemory.error())
						break;
					buffer.push_back(b);
				}
				s = {buffer.data(), buffer.size()};
			} else if (object.matchFullString("exec-file")) {
				// TODO: consider the annex (= process ID).
				s = frg::span<const std::byte>{
				    reinterpret_cast<const std::byte *>(path_.data()), path_.size()
				};
			} else if (object.matchFullString("features") && annex.matchFullString("target.xml")) {
				const char *xml = "<target version=\"1.0\">"
#if defined(__x86_64__)
				                  "<architecture>i386:x86-64</architecture>"
#elif defined(__aarch64__)
				                  "<architecture>aarch64</architecture>"
#else
#error Unknown architecture
#endif
				                  "</target>";
				s = frg::span<const std::byte>{
				    reinterpret_cast<const std::byte *>(xml), strlen(xml)
				};
			}

			if (s) {
				if (offset >= s->size()) {
					// End-of-object (offset beyond object size).
					resp.appendString("l");
				} else if (offset + length >= s->size()) {
					// End-of-object.
					resp.appendString("l");
					resp.appendBinary(s->subspan(offset));
				} else {
					// More data available.
					resp.appendString("m");
					resp.appendBinary(s->subspan(offset, length));
				}
			}
		} else if (req.matchString("Attached")) {
			// Return an indication of whether the remote server attached to an existing process or
			// created a new process.
			resp.appendString("1"); // 1: The remote server attached to an existing process.
		} else if (req.matchString("TStatus")) {
			// Ask the stub if there is a trace experiment running right now.
			// We don't currently even support trace points.
			resp.appendString("T0;tnotrun:0"
			); // no trace is currently running and none has been run yet
		} else if (req.matchString("Symbol::")) {
			// Notify the target (this) that GDB is prepared to serve symbol lookup requests.
			// Accept requests from the target (this) for the values of symbols.
			resp.appendString("OK"); // We don't plan on making any requests.
		} else if (req.matchString("L")) {
			// Obtain thread information from RTOS.
			// We don't return info about threads.
			resp.appendString("qM001"); // return 0 threads (2 hex digits) with no intention to
			                            // return more (1, as the last hex digit)
		} else {
			co_return ProtocolError::unknownPacket;
		}
	} else if (req.matchString("v")) { // Multi-letter requests.
		if (req.matchString("MustReplyEmpty")) {
			// Must be handled like unknown v packets (but do not complain).
			req = {}; // Fully consume the packet.
		} else if (req.matchString("Cont")) {
			if (req.matchString("?")) {
				// Request a list of actions supported by the 'vCont' packet
				resp.appendString(""); // We don't support this.
			} else {
				co_return ProtocolError::unknownPacket;
			}
		} else {
			co_return ProtocolError::unknownPacket;
		}
	} else if (req.matchString("D")) {
		// Detach GDB from the remote system.
		resp.appendString("OK"); // return success
	} else {
		co_return ProtocolError::unknownPacket;
	}

	co_return {};
}

} // anonymous namespace

static bool launched = false;

void launchGdbServer(Process *process) {
	if (launched)
		return;
	launched = true;
	async::detach([](Process *process) -> async::result<void> {
		std::cout << "posix: Starting GDB server" << std::endl;

		auto root = rootPath();
		auto fileOrError = co_await open(root, root, "dev/ttyS0", process);
		if (!fileOrError) {
			std::cout << "posix, gdbserver: Could not open /dev/ttyS0" << std::endl;
			co_return;
		}

		GdbServer server{process, fileOrError.value()};
		co_await server.run();
	}(process));
}
