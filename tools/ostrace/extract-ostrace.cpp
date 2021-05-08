#include <err.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#include <bragi/helpers-std.hpp>
#include <CLI/App.hpp>
#include <CLI/Formatter.hpp>
#include <CLI/Config.hpp>
#include <frg/span.hpp>
#include <ostrace.bragi.hpp>

enum class ExtractMode {
	none,
	eventOnly,
	specificItem
};


std::unordered_map<std::string, ExtractMode> stringToExtractMode{
	{"event-only", ExtractMode::eventOnly},
	{"specific-item", ExtractMode::specificItem},
};

int main(int argc, char **argv) {
	ExtractMode mode{};
	std::string path{"virtio-trace.bin"};
	std::string eventName;
	std::string itemName;

	CLI::App app{"extract-ostrace: extract records from ostrace logs"};
	app.add_option("path", path, "Path to the input file");
	app.add_option("-m,--mode", mode, "Operational mode")
		->required()
		->transform(CLI::CheckedTransformer(stringToExtractMode));
	app.add_option("-e,--event", eventName, "Match only specific events");
	app.add_option("-i,--item", itemName, "Extract a specific item");
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

	auto ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if(ptr == MAP_FAILED) {
		ptr = nullptr;
		err(1, "failed to mmap file");
	}

	close(fd);

	frg::span<const char> buffer{reinterpret_cast<const char *>(ptr),
			static_cast<size_t>(st.st_size)};

	uint64_t filteredEventId = 0;
	uint64_t desiredItemId = 0;

	std::vector<uint64_t> ts;
	std::vector<uint64_t> value;

	auto extractRecord = [&] () -> bool {
		auto preamble = bragi::read_preamble(buffer);
		if(preamble.error()) {
			warnx("halting due to broken preamble");
			return false;
		}

		// All records have a head size of 8.
		auto head_span = buffer.subspan(0, 8);
		auto tail_span = buffer.subspan(8, preamble.tail_size());

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::EventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::EventRecord>(
					head_span, tail_span);
			if(!maybeRecord) {
				warnx("halting due to broken record");
				return false;
			}
			auto &record = maybeRecord.value();

			if(record.id() == filteredEventId) {
				switch(mode) {
				case ExtractMode::eventOnly:
					ts.push_back(record.ts());
					break:
				case ExtractMode::specificItem:
					for(size_t i = 0; i < record.ctrs_size(); ++i) {
						if(record.ctrs(i).id() != desiredItemId)
							continue;
						ts.push_back(record.ts());
						value.push_back(record.ctrs(i).value());
					}
					break;
				default:;
				}
			}
		} break;
		case bragi::message_id<managarm::ostrace::AnnounceEventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::AnnounceEventRecord>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(record.name() == eventName)
				filteredEventId = record.id();
		} break;
		case bragi::message_id<managarm::ostrace::AnnounceItemRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::AnnounceItemRecord>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(record.name() == itemName)
				desiredItemId = record.id();
		} break;
		default:
			warnx("halting due to unexpected message ID %u", preamble.id());
			return false;
		}

		buffer = buffer.subspan(8 + preamble.tail_size());
		return true;
	};

	size_t nRecords = 0;
	while (buffer.size()) {
		if(!extractRecord())
			break;
		++nRecords;
	}

	std::cout << "{\n";
	std::cout << "\"ts\": [";
	for(size_t i = 0; i < ts.size(); ++i)
		std::cout << (i ? ", " : "") << ts[i];
	std::cout << "]\n";
	if(mode == ExtractMode::specificItem) {
		std::cout << ",\n";
		std::cout << "\"value\": [";
		for(size_t i = 0; i < value.size(); ++i)
			std::cout << (i ? ", " : "") << value[i];
		std::cout << "]\n";
	}
	std::cout << "}" << std::endl;

	std::cerr << "extracted " << nRecords << " records"
			<< " (" << buffer.size() << " bytes remain)" << std::endl;
	std::cerr << "found " << ts.size() << " matches" << std::endl;
}
