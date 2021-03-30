#include <err.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#include <bragi/helpers-std.hpp>
#include <frg/span.hpp>
#include <ostrace.bragi.hpp>

int main() {
	int fd = open("virtio-trace.bin", O_RDONLY);
	if(fd < 0)
		err(1, "failed to open file");

	struct stat st;
	if(fstat(fd, &st) < 0) {
		err(1, "failed to stat file");
	}

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

	size_t n_records = 0;
	while (buffer.size()) {
		auto preamble = bragi::read_preamble(buffer);
		assert(!preamble.error());

		// All records have a head size of 8.
		auto head_span = buffer.subspan(0, 8);
		auto tail_span = buffer.subspan(8, preamble.tail_size());

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::EventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::EventRecord>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(record.id() == filteredEventId) {
				for(size_t i = 0; i < record.ctrs_size(); ++i) {
					if(record.ctrs(i).id() != desiredItemId)
						continue;
					ts.push_back(record.ts());
					value.push_back(record.ctrs(i).value());
				}
			}
		} break;
		case bragi::message_id<managarm::ostrace::AnnounceEventRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::AnnounceEventRecord>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(record.name() == "libblockfs.read")
				filteredEventId = record.id();
		} break;
		case bragi::message_id<managarm::ostrace::AnnounceItemRecord>: {
			auto maybeRecord = bragi::parse_head_tail<managarm::ostrace::AnnounceItemRecord>(
					head_span, tail_span);
			assert(maybeRecord);
			auto &record = maybeRecord.value();

			if(record.name() == "time")
				desiredItemId = record.id();
		} break;
		default:
			errx(1, "unexpected message ID");
		}

		buffer = buffer.subspan(8 + preamble.tail_size());
		++n_records;
	}

	std::cout << "{\n";
	std::cout << "\"ts\": [";
	for(size_t i = 0; i < ts.size(); ++i)
		std::cout << (i ? ", " : "") << ts[i];
	std::cout << "],\n";
	std::cout << "\"value\": [";
	for(size_t i = 0; i < value.size(); ++i)
		std::cout << (i ? ", " : "") << value[i];
	std::cout << "]\n";
	std::cout << "}" << std::endl;

	std::cerr << "extracted " << n_records << " records" << std::endl;
}
