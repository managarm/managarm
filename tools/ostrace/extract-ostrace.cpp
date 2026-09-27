#include <err.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <queue>
#include <variant>

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
concept Policy = requires(T &a, managarm::ostrace::EventRecord &event, managarm::ostrace::Definition &def, managarm::ostrace::UintAttribute &uintAttr, managarm::ostrace::BufferAttribute &bufferAttr, uint64_t source, size_t pass) {
	{ a.onEvent(event, source, pass) } -> std::same_as<bool>;
	{ a.onDefinition(def, pass) } -> std::same_as<bool>;
	{ a.onEndOfRecord(pass) } -> std::same_as<bool>;

	{ a.onUintAttribute(uintAttr, pass) } -> std::same_as<bool>;
	{ a.onBufferAttribute(bufferAttr, pass) } -> std::same_as<bool>;

	{ a.passes() } -> std::same_as<size_t>;
	{ a.reset() } -> std::same_as<void>;
	{ a.finish() } -> std::same_as<void>;

	requires std::is_same_v<decltype(a.parsedRecords), size_t>;
	requires std::is_same_v<decltype(a.terms), const std::unordered_map<uint64_t, std::string> *>;
};

struct JsonPolicy {
	bool onEvent(managarm::ostrace::EventRecord &record, uint64_t source, size_t) {
		std::cout << "{\"_event\":\"" << terms->at(record.id()) << "\",\"_ts\":" << record.ts()
			<< ",\"_source\":" << source;
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
		std::cout << ",\"" << terms->at(record.id()) << "\":" << record.v();
		return true;
	}

	bool onBufferAttribute(managarm::ostrace::BufferAttribute &record, size_t) {
		std::cout << ",\"" << terms->at(record.id()) << "\": \"<buffer of size " << record.buffer().size() << ">\"";
		return true;
	}

	size_t passes() {
		return 1;
	}

	void reset() {

	}

	void finish() {

	}

	const std::unordered_map<uint64_t, std::string> *terms = nullptr;
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

	bool onEvent(managarm::ostrace::EventRecord &record, uint64_t, size_t) {
		if(requests.contains(terms->at(record.id())))
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
		if(terms->at(record.id()) == "pid") {
			state_.last_pid = record.v();
		} else if(terms->at(record.id()) == "time") {
			state_.last_request_ts = record.v();
		} else if(terms->at(record.id()) == "request") {
			state_.last_request = record.v();
		}
		return true;
	}

	bool onBufferAttribute(managarm::ostrace::BufferAttribute &record, size_t pass) {
		auto name = terms->at(record.id());

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

		// Frame IDs are indices into the pcap, starting at 1.
		if(pass == 0) {
			if(state_.last_request) {
				// Replies whose request was lost cannot be part of a conversation.
				auto it = requests_.find(metadata);
				if(it == requests_.end()) {
					++orphans_;
					return true;
				}
				it->second.second = frame_id;
			} else {
				requests_.insert({metadata, {frame_id, 0}});
			}
		} else {
			auto it = requests_.find(metadata);
			// Pass 0 dropped replies that precede their request.
			if(state_.last_request && (it == requests_.end() || it->second.first >= frame_id))
				return true;
			auto convo = it->second;
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

	void finish() {
		if(orphans_)
			warnx("dropped %zu replies without a request", orphans_);
	}

	size_t parsedRecords;
	const std::unordered_map<uint64_t, std::string> *terms = nullptr;

private:
	int pcapfd_;
	size_t frame_id = 1;
	size_t orphans_ = 0;

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

	// A page of a writer. The page contains the events first_event to first_event + num_events - 1
	// of the writer.
	struct Page {
		uint64_t firstEvent;
		uint64_t numEvents;
		frg::span<const char> buffer;
	};

	struct Writer {
		uint64_t source;
		// Sorted by firstEvent.
		std::vector<Page> pages;
	};

	// Gaps in the sequence numbers of frames and in the event numbers of writers show where
	// data was lost.
	struct LossStats {
		std::vector<uint64_t> frameSeqs;
		size_t brokenPages = 0;
		size_t unorderedEvents = 0;

		static uint64_t countGaps(std::vector<uint64_t> &seqs) {
			std::ranges::sort(seqs);
			auto [first, last] = std::ranges::unique(seqs);
			seqs.erase(first, last);
			// Sequence numbers start at zero.
			return seqs.empty() ? 0 : seqs.back() + 1 - seqs.size();
		}

		static uint64_t countLostEvents(const std::vector<Page> &pages) {
			// Event numbers start at zero.
			uint64_t lost = 0;
			uint64_t next = 0;
			for(auto &page : pages) {
				if(page.firstEvent > next)
					lost += page.firstEvent - next;
				next = std::max(next, page.firstEvent + page.numEvents);
			}
			return lost;
		}

		void report(const std::vector<Writer> &writers) {
			std::cerr << "lost " << countGaps(frameSeqs) << " frames in the kernel" << std::endl;
			std::map<uint64_t, uint64_t> lostEvents;
			for(auto &writer : writers)
				lostEvents[writer.source] += countLostEvents(writer.pages);
			for(auto &[source, lost] : lostEvents)
				std::cerr << "lost " << lost << " events of source " << source << std::endl;
			std::cerr << "skipped " << brokenPages << " broken pages" << std::endl;
			if(unorderedEvents)
				std::cerr << "merged " << unorderedEvents << " events out of timestamp order"
					<< std::endl;
		}
	};

	using Record = std::variant<managarm::ostrace::Definition, managarm::ostrace::EndOfRecord,
			managarm::ostrace::EventRecord, managarm::ostrace::UintAttribute,
			managarm::ostrace::BufferAttribute>;

	auto parseRecord = [] (frg::span<const char> &buffer) -> std::optional<Record> {
		auto preamble = bragi::read_preamble(buffer);
		if(preamble.error()) {
			warnx("broken preamble");
			return std::nullopt;
		}

		// All records have a head size of 8.
		auto head_span = buffer.subspan(0, 8);
		if(buffer.size() < 8 + preamble.tail_size()) {
			warnx("truncated record");
			return std::nullopt;
		}
		auto tail_span = buffer.subspan(8, preamble.tail_size());

		auto parse = [&]<typename R>() -> std::optional<Record> {
			auto maybeRecord = bragi::parse_head_tail<R>(head_span, tail_span);
			if(!maybeRecord) {
				warnx("broken record");
				return std::nullopt;
			}
			buffer = buffer.subspan(8 + preamble.tail_size());
			return std::move(*maybeRecord);
		};

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::Definition>:
			return parse.template operator()<managarm::ostrace::Definition>();
		case bragi::message_id<managarm::ostrace::EndOfRecord>:
			return parse.template operator()<managarm::ostrace::EndOfRecord>();
		case bragi::message_id<managarm::ostrace::EventRecord>:
			return parse.template operator()<managarm::ostrace::EventRecord>();
		case bragi::message_id<managarm::ostrace::UintAttribute>:
			return parse.template operator()<managarm::ostrace::UintAttribute>();
		case bragi::message_id<managarm::ostrace::BufferAttribute>:
			return parse.template operator()<managarm::ostrace::BufferAttribute>();
		default:
			warnx("unexpected message ID %u", preamble.id());
			return std::nullopt;
		}
	};

	// Only parses the Frames; the pages are parsed while merging.
	// Frames are written by the kernel, so a broken page does not affect the following frames.
	auto indexFrames = [] (frg::span<const char> &buffer, LossStats &stats) -> std::vector<Writer> {
		constexpr size_t frameHeadSize = bragi::head_size<managarm::ostrace::Frame>;

		// Indexed by source and writer.
		std::map<std::pair<uint64_t, uint64_t>, Writer> writers;
		while (buffer.size()) {
			if(buffer.size() < frameHeadSize) {
				warnx("halting due to truncated frame");
				break;
			}
			auto frame = bragi::parse_head_only<managarm::ostrace::Frame>(
					buffer.subspan(0, frameHeadSize));
			if(!frame) {
				warnx("halting due to broken frame");
				break;
			}
			if(buffer.size() - frameHeadSize < frame->size()) {
				warnx("halting due to truncated frame");
				break;
			}

			stats.frameSeqs.push_back(frame->seq());
			auto &writer = writers[{frame->source(), frame->writer()}];
			writer.source = frame->source();
			writer.pages.push_back({frame->first_event(), frame->num_events(),
					buffer.subspan(frameHeadSize, frame->size())});

			buffer = buffer.subspan(frameHeadSize + frame->size());
		}

		std::vector<Writer> result;
		for(auto &[key, writer] : writers) {
			// Stable, such that pages without event numbers stay in the order of their frames.
			std::ranges::stable_sort(writer.pages, {}, &Page::firstEvent);
			result.push_back(std::move(writer));
		}
		return result;
	};

	// Reads the pages of a writer event by event.
	struct Cursor {
		const Writer *writer;
		size_t nextPage = 0;
		// Records of the current page that were not read yet.
		frg::span<const char> rest;
		// IDs are local to each page.
		std::unordered_map<uint64_t, std::string> terms;
		// Records of the current event, including the Definitions that precede it.
		std::vector<Record> event;
		uint64_t ts = 0;
	};

	// Reads the next complete event of the writer, skipping broken pages.
	// Returns false if the writer has no more events.
	auto advance = [&parseRecord] (Cursor &cursor, LossStats *stats) -> bool {
		bool inEvent = false;

		// Pages define all IDs before using them.
		auto isDefined = [&] (uint64_t id) -> bool {
			if(!cursor.terms.contains(id)) {
				warnx("use of undefined ID %lu", id);
				return false;
			}
			return true;
		};

		// Returns false if the record breaks the page.
		auto accept = [&] (Record &record) -> bool {
			if(auto def = std::get_if<managarm::ostrace::Definition>(&record)) {
				cursor.terms[def->id()] = def->name();
				return true;
			}
			if(auto event = std::get_if<managarm::ostrace::EventRecord>(&record)) {
				if(inEvent) {
					warnx("missing EndOfRecord");
					return false;
				}
				if(!isDefined(event->id()))
					return false;
				if(stats && event->ts() < cursor.ts)
					++stats->unorderedEvents;
				inEvent = true;
				cursor.ts = event->ts();
				return true;
			}
			if(!inEvent) {
				warnx("record outside of an event");
				return false;
			}
			if(auto attr = std::get_if<managarm::ostrace::UintAttribute>(&record))
				return isDefined(attr->id());
			if(auto attr = std::get_if<managarm::ostrace::BufferAttribute>(&record))
				return isDefined(attr->id());
			return true;
		};

		auto skipPage = [&] {
			warnx("skipping broken page of source %lu", cursor.writer->source);
			if(stats)
				++stats->brokenPages;
			cursor.rest = {};
			inEvent = false;
		};

		cursor.event.clear();
		while (true) {
			if(!cursor.rest.size()) {
				if(inEvent) {
					warnx("truncated event");
					skipPage();
				}
				if(cursor.nextPage == cursor.writer->pages.size())
					return false;
				cursor.rest = cursor.writer->pages[cursor.nextPage++].buffer;
				cursor.terms.clear();
				cursor.event.clear();
				continue;
			}

			auto record = parseRecord(cursor.rest);
			if(!record || !accept(*record)) {
				skipPage();
				continue;
			}
			bool isEnd = std::holds_alternative<managarm::ostrace::EndOfRecord>(*record);
			cursor.event.push_back(std::move(*record));
			if(isEnd)
				return true;
		}
	};

	auto dispatch = []<Policy T>(T &policy, Record &record, uint64_t source, size_t pass) -> bool {
		return std::visit([&] (auto &r) -> bool {
			using R = std::remove_cvref_t<decltype(r)>;
			if constexpr (std::is_same_v<R, managarm::ostrace::Definition>)
				return policy.onDefinition(r, pass);
			else if constexpr (std::is_same_v<R, managarm::ostrace::EndOfRecord>)
				return policy.onEndOfRecord(pass);
			else if constexpr (std::is_same_v<R, managarm::ostrace::EventRecord>)
				return policy.onEvent(r, source, pass);
			else if constexpr (std::is_same_v<R, managarm::ostrace::UintAttribute>)
				return policy.onUintAttribute(r, pass);
			else
				return policy.onBufferAttribute(r, pass);
		}, record);
	};

	// The events of each writer are in timestamp order, so a k-way merge of the writers orders
	// all events while holding only one event per writer in memory.
	auto merge = [&advance, &dispatch]<Policy T>(T &policy, const std::vector<Writer> &writers,
			size_t pass, LossStats *stats) {
		std::vector<Cursor> cursors;
		for(auto &writer : writers)
			cursors.push_back({.writer = &writer});

		// Min-heap of the timestamps of the cursors' events. The cursor index breaks ties.
		using Entry = std::pair<uint64_t, size_t>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
		for(size_t i = 0; i < cursors.size(); ++i) {
			if(advance(cursors[i], stats))
				heap.push({cursors[i].ts, i});
		}

		while (!heap.empty()) {
			auto i = heap.top().second;
			heap.pop();
			auto &cursor = cursors[i];

			policy.terms = &cursor.terms;
			for(auto &record : cursor.event) {
				if(!dispatch(policy, record, cursor.writer->source, pass))
					warnx("failed to handle record");
				++policy.parsedRecords;
			}
			policy.terms = nullptr;

			if(advance(cursor, stats))
				heap.push({cursor.ts, i});
		}
	};

	auto parseWithPolicy = [&indexFrames, &merge]<Policy T>(T &policy,
			frg::span<const char> &fileBuffer) {
		LossStats stats;
		auto writers = indexFrames(fileBuffer, stats);
		for(size_t pass = 0; pass < policy.passes(); pass++) {
			policy.parsedRecords = 0;
			policy.reset();

			merge(policy, writers, pass, pass ? nullptr : &stats);
		}
		policy.finish();

		std::cerr << "extracted " << policy.parsedRecords << " records"
			<< " (" << fileBuffer.size() << " bytes remain)" << std::endl;
		stats.report(writers);
	};

	if(pcap) {
		auto policy = WiresharkPolicy{};
		parseWithPolicy(policy, fileBuffer);
	} else {
		auto policy = JsonPolicy{};
		parseWithPolicy(policy, fileBuffer);
	}
}
