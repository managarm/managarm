#include <err.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bragi/helpers-std.hpp>
#include <CLI/App.hpp>
#include <CLI/Formatter.hpp>
#include <CLI/Config.hpp>
#include <frg/span.hpp>
#include <ostrace.bragi.hpp>

void FRG_INTF(panic)(const char *cstring) {
	std::cout << "PANIC: " << cstring << std::endl;
}

namespace {

template<typename T>
concept Policy = requires(T &a, managarm::ostrace::EventRecord &event, managarm::ostrace::Definition &def, managarm::ostrace::UintAttribute &uintAttr, managarm::ostrace::BufferAttribute &bufferAttr, size_t pass) {
	{ a.onEvent(event, pass) } -> std::same_as<bool>;
	{ a.onDefinition(def, pass) } -> std::same_as<bool>;
	{ a.onEndOfRecord(pass) } -> std::same_as<bool>;

	{ a.onUintAttribute(uintAttr, pass) } -> std::same_as<bool>;
	{ a.onBufferAttribute(bufferAttr, pass) } -> std::same_as<bool>;

	{ a.passes() } -> std::same_as<size_t>;
	{ a.reset() } -> std::same_as<void>;

	requires std::is_same_v<decltype(a.parsedRecords), size_t>;
	requires std::is_same_v<decltype(a.terms), std::unordered_map<uint64_t, std::string>>;
};

struct JsonPolicy {
	bool onEvent(managarm::ostrace::EventRecord &record, size_t) {
		std::cout << "{\"_event\":\"" << terms.at(record.id()) << "\",\"_ts\":" << record.ts();
		return true;
	}

	bool onDefinition(managarm::ostrace::Definition &, size_t) {
		return true;
	}

	bool onEndOfRecord(size_t) {
		std::cout << "}\n";
		return true;
	}

	bool onUintAttribute(managarm::ostrace::UintAttribute &record, size_t) {
		std::cout << ",\"" << terms.at(record.id()) << "\":" << record.v();
		return true;
	}

	bool onBufferAttribute(managarm::ostrace::BufferAttribute &record, size_t) {
		std::cout << ",\"" << terms.at(record.id()) << "\": \"<buffer of size " << record.buffer().size() << ">\"";
		return true;
	}

	size_t passes() {
		return 1;
	}

	void reset() {

	}

	std::unordered_map<uint64_t, std::string> terms;
	size_t parsedRecords;
};

struct WiresharkPolicy {
	WiresharkPolicy() {
		pcapfd_ = open("bragi.pcap", O_CREAT | O_TRUNC | O_RDWR, 0666);
		if(pcapfd_ < 0)
			err(1, "failed to open pcap file");

		struct pcap_hdr_s {
			uint32_t magic_number;
			uint16_t version_major;
			uint16_t version_minor;
			int32_t thiszone;
			uint32_t sigfigs;
			uint32_t snaplen;
			uint32_t network;
		} pcap_hdr {
			.magic_number = 0xa1b2c3d4,
			.version_major = 2,
			.version_minor = 4,
			.thiszone = 0,
			.sigfigs = 0,
			.snaplen = 65536,
			.network = 147
		};

		write(pcapfd_, &pcap_hdr, sizeof(pcap_hdr));
	}

	std::set<std::string_view> requests = {
		"posix.request",
		"fs.request",
	};

	bool onEvent(managarm::ostrace::EventRecord &record, size_t) {
		if(requests.contains(terms.at(record.id())))
			state_.ts = record.ts();
		return true;
	}

	bool onDefinition(managarm::ostrace::Definition &, size_t) {
		return true;
	}

	bool onEndOfRecord(size_t) {
		state_ = {};
		return true;
	}

	bool onUintAttribute(managarm::ostrace::UintAttribute &record, size_t) {
		if(terms.at(record.id()) == "pid") {
			state_.last_pid = record.v();
		} else if(terms.at(record.id()) == "time") {
			state_.last_request_ts = record.v();
		} else if(terms.at(record.id()) == "request") {
			state_.last_request = record.v();
		}
		return true;
	}

	bool onBufferAttribute(managarm::ostrace::BufferAttribute &record, size_t pass) {
		auto name = terms.at(record.id());

		if(!name.starts_with("0x") || name.size() > 10)
			return true;

		uint32_t proto_hash = std::stoul(name, nullptr, 16);

		bragi_msg_metadata metadata {state_.last_pid, 0, 0};
		if(state_.last_request) {
			metadata.last_request = state_.last_request;
			metadata.last_request_ts = state_.last_request_ts;
		} else {
			metadata.last_request = *reinterpret_cast<uint32_t *>(record.buffer().data());
			metadata.last_request_ts = state_.ts;
		}

		if(pass == 0) {
			if(state_.last_request) {
				if(requests_.contains(metadata)) {
					auto &convo = requests_.at(metadata);
					convo.second = frame_id;
				}
			} else {
				requests_.insert({metadata, {frame_id, 0}});
			}
		} else {
			if(!requests_.contains(metadata)) {
				printf("No metadata found for PID %u Request %u TS %zu\n",
					metadata.last_pid, metadata.last_request, metadata.last_request_ts);
				exit(1);
			}
			auto convo = requests_.at(metadata);
			size_t request_time = 0;
			if(state_.ts > metadata.last_request_ts)
				request_time = state_.ts - metadata.last_request_ts;
			uint32_t packet_size = record.buffer().size()
				+ sizeof(proto_hash) + sizeof(state_.last_pid) + sizeof(convo.first) + sizeof(convo.second) + sizeof(request_time);

			struct pcaprec_hdr_s {
				uint32_t ts_sec;
				uint32_t ts_usec;
				uint32_t incl_len;
				uint32_t orig_len;
			} rec_hdr {
				.ts_sec = static_cast<uint32_t>(state_.ts / 1'000'000'000),
				.ts_usec = static_cast<uint32_t>((state_.ts % 1'000'000'000) / 1'000),
				.incl_len = packet_size,
				.orig_len = packet_size,
			};

			write(pcapfd_, &rec_hdr, sizeof(rec_hdr));
			write(pcapfd_, &proto_hash, sizeof(proto_hash));
			write(pcapfd_, &state_.last_pid, sizeof(state_.last_pid));
			write(pcapfd_, &convo.first, sizeof(convo.first));
			write(pcapfd_, &convo.second, sizeof(convo.second));
			write(pcapfd_, &request_time, sizeof(request_time));
			write(pcapfd_, record.buffer().data(), record.buffer().size());
		}

		frame_id++;

		return true;
	}

	size_t passes() {
		return 2;
	}

	void reset() {
		state_ = {};
		frame_id = 1;
	}

	size_t parsedRecords;
	std::unordered_map<uint64_t, std::string> terms;

private:
	int pcapfd_;
	size_t frame_id = 1;

	struct pcap_packet_state {
		pid_t last_pid = 0;
		uint32_t last_request = 0;
		uint64_t last_request_ts = 0;
		uint64_t ts = 0;
	} state_;

	struct bragi_msg_metadata {
		pid_t last_pid;
		uint32_t last_request;
		uint64_t last_request_ts;

		auto operator<=>(const bragi_msg_metadata &) const = default;
	};

	std::map<bragi_msg_metadata, std::pair<size_t, size_t>> requests_;
};

} // namespace

int main(int argc, char **argv) {
	std::string path{"virtio-trace.bin"};
	bool pcap = false;

	CLI::App app{"extract-ostrace: extract records from ostrace logs"};
	app.add_flag("--pcap", pcap, "Produce a bragi.pcap");
	app.add_option("path", path, "Path to the input file");
	CLI11_PARSE(app, argc, argv);

	int fd = open(path.c_str(), O_RDONLY);
	if(fd < 0)
		err(1, "failed to open input file %s", path.c_str());

	struct stat st;
	if(fstat(fd, &st) < 0) {
		err(1, "failed to stat file");
	}
	if(!st.st_size)
		err(1, "input file is empty");

	auto ptr = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if(ptr == MAP_FAILED) {
		ptr = nullptr;
		err(1, "failed to mmap file");
	}

	close(fd);

	frg::span<const char> fileBuffer{reinterpret_cast<const char *>(ptr),
			static_cast<size_t>(st.st_size)};

	auto handleMessage = []<Policy T>(T &policy, frg::span<const char> &buffer, size_t pass) -> bool {
		auto preamble = bragi::read_preamble(buffer);
		if(preamble.error()) {
			warnx("halting due to broken preamble");
			return false;
		}

		// All records have a head size of 8.
		auto head_span = buffer.subspan(0, 8);
		if(buffer.size() < 8 + preamble.tail_size()) {
			warnx("halting due to truncated record head");
			return false;
		}
		auto tail_span = buffer.subspan(8, preamble.tail_size());

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::Definition>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::Definition>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			policy.terms[record.id()] = record.name();
			if(!policy.onDefinition(record, pass)) {
				warnx("failed to parse Definition");
				return false;
			}
		} break;
		case bragi::message_id<managarm::ostrace::EndOfRecord>:
			if(!policy.onEndOfRecord(pass)) {
				warnx("failed to parse EndOfRecord");
				return false;
			}
			break;
		case bragi::message_id<managarm::ostrace::EventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::EventRecord>(
					head_span, tail_span);
			if(!maybeRecord) {
				warnx("halting due to broken record");
				return false;
			}
			auto &record = maybeRecord.value();

			if(!policy.onEvent(record, pass)) {
				warnx("failed to parse EventRecord");
				return false;
			}
		} break;
		case bragi::message_id<managarm::ostrace::UintAttribute>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::UintAttribute>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(!policy.onUintAttribute(record, pass)) {
				warnx("failed to parse UintAttribute");
				return false;
			}
		} break;
		case bragi::message_id<managarm::ostrace::BufferAttribute>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::BufferAttribute>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(!policy.onBufferAttribute(record, pass)) {
				warnx("failed to parse BufferAttribute");
				return false;
			}
		} break;
		default:
			warnx("halting due to unexpected message ID %u", preamble.id());
			return false;
		}

		if(buffer.size() >= 8 + preamble.tail_size()) {
			buffer = buffer.subspan(8 + preamble.tail_size());
			return true;
		}

		return false;
	};

	auto extractRecords = [&handleMessage]<Policy T>(T &policy, frg::span<const char> &bufferView, size_t pass) -> bool {
		struct Header {
			uint32_t size;
		};

		if (bufferView.size() < sizeof(Header)) {
			std::cerr << "failed to extract header" << std::endl;
			return false;
		}
		Header hdr;
		memcpy(&hdr, bufferView.data(), sizeof(Header));

		auto buffer = bufferView.subspan(sizeof(Header), hdr.size);
		while (buffer.size()) {
			if(!handleMessage(policy, buffer, pass)) {
				return false;
			}
			++policy.parsedRecords;
		}

		bufferView = bufferView.subspan(sizeof(Header) + hdr.size);
		return true;
	};

	auto parseWithPolicy = [&extractRecords]<Policy T>(T &policy, frg::span<const char> &fileBuffer) {
		for(size_t pass = 0; pass < policy.passes(); pass++) {
			auto bufferView = fileBuffer.subspan(0);
			policy.parsedRecords = 0;
			policy.reset();

			while (bufferView.size()) {
				if (!extractRecords(policy, bufferView, pass))
					break;
			}

			if(pass == policy.passes() - 1)
				fileBuffer = bufferView;
		}

		std::cerr << "extracted " << policy.parsedRecords << " records"
			<< " (" << fileBuffer.size() << " bytes remain)" << std::endl;
	};

	if(pcap) {
		auto policy = WiresharkPolicy{};
		parseWithPolicy(policy, fileBuffer);
	} else {
		auto policy = JsonPolicy{};
		parseWithPolicy(policy, fileBuffer);
	}
}
