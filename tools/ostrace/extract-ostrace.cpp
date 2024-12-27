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

int main(int argc, char **argv) {
	std::string path{"virtio-trace.bin"};

	CLI::App app{"extract-ostrace: extract records from ostrace logs"};
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

	auto ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if(ptr == MAP_FAILED) {
		ptr = nullptr;
		err(1, "failed to mmap file");
	}

	close(fd);

	frg::span<const char> fileBuffer{reinterpret_cast<const char *>(ptr),
			static_cast<size_t>(st.st_size)};

	size_t nRecords = 0;

	std::vector<uint64_t> ts;
	std::vector<uint64_t> value;
	std::unordered_map<uint64_t, std::string> terms;

	auto extractMsg = [&] (frg::span<const char> &buffer) -> bool {
		auto preamble = bragi::read_preamble(buffer);
		if(preamble.error()) {
			warnx("halting due to broken preamble");
			return false;
		}

		// All records have a head size of 8.
		auto head_span = buffer.subspan(0, 8);
		auto tail_span = buffer.subspan(8, preamble.tail_size());

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::Definition>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::Definition>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			terms[record.id()] = record.name();
		} break;
		case bragi::message_id<managarm::ostrace::EndOfRecord>:
			std::cout << "}\n";
			break;
		case bragi::message_id<managarm::ostrace::EventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::EventRecord>(
					head_span, tail_span);
			if(!maybeRecord) {
				warnx("halting due to broken record");
				return false;
			}
			auto &record = maybeRecord.value();

			std::cout << "{\"_event\":\"" << terms.at(record.id()) << "\",\"_ts\":" << record.ts();
		} break;
		case bragi::message_id<managarm::ostrace::UintAttribute>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::UintAttribute>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			std::cout << ",\"" << terms.at(record.id()) << "\":" << record.v();
		} break;
		default:
			warnx("halting due to unexpected message ID %u", preamble.id());
			return false;
		}

		buffer = buffer.subspan(8 + preamble.tail_size());
		return true;
	};

	auto extractRecords = [&] () -> bool {
		struct Header {
			uint32_t size;
		};

		if (fileBuffer.size() < sizeof(Header)) {
			std::cerr << "failed to extract header" << std::endl;
			return false;
		}
		Header hdr;
		memcpy(&hdr, fileBuffer.data(), sizeof(Header));

		auto buffer = fileBuffer.subspan(sizeof(Header), hdr.size);
		while (buffer.size()) {
			if(!extractMsg(buffer))
				return false;
			++nRecords;
		}

		fileBuffer = fileBuffer.subspan(sizeof(Header) + hdr.size);
		return true;
	};

	while (fileBuffer.size()) {
		if (!extractRecords())
			break;
	}

	std::cerr << "extracted " << nRecords << " records"
			<< " (" << fileBuffer.size() << " bytes remain)" << std::endl;
}
