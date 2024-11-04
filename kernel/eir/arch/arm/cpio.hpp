#pragma once

#include <frg/span.hpp>
#include <frg/string.hpp>

struct CpioHeader {
	char magic[6];
	char inode[8];
	char mode[8];
	char uid[8];
	char gid[8];
	char numLinks[8];
	char mtime[8];
	char fileSize[8];
	char devMajor[8];
	char devMinor[8];
	char rdevMajor[8];
	char rdevMinor[8];
	char nameSize[8];
	char check[8];
};

struct CpioFile {
	frg::string_view name;
	frg::span<uint8_t> data;
};

struct CpioRange {
	struct Iterator {
		Iterator(void *ptr) : ptr_{static_cast<uint8_t *>(ptr)} {}

		Iterator &operator++() {
			next();
			return *this;
		}

		CpioFile operator*() { return parse(); }

		uint8_t *get() { return ptr_; }

		bool operator==(const Iterator &other) const { return other.ptr_ == ptr_; }

	  private:
		uint32_t parseHex(const char *c, int n) {
			uint32_t v = 0;

			while (n--) {
				v <<= 4;
				if (*c >= '0' && *c <= '9')
					v |= *c - '0';
				if (*c >= 'a' && *c <= 'f')
					v |= *c - 'a' + 10;
				if (*c >= 'A' && *c <= 'F')
					v |= *c - 'A' + 10;
				c++;
			}

			return v;
		}

		CpioFile parse() {
			CpioHeader hdr;
			memcpy(&hdr, ptr_, sizeof(CpioHeader));

			auto magic = parseHex(hdr.magic, 6);
			assert(magic == 0x070701 || magic == 0x070702);

			auto nameSize = parseHex(hdr.nameSize, 8);
			auto fileSize = parseHex(hdr.fileSize, 8);

			frg::string_view path{
			    reinterpret_cast<char *>(ptr_) + sizeof(CpioHeader), nameSize - 1
			};
			frg::span<uint8_t> data{
			    ptr_ + ((sizeof(CpioHeader) + nameSize + 3) & ~uint32_t{3}), fileSize
			};

			return {path, data};
		}

		void next() {
			CpioHeader hdr;
			memcpy(&hdr, ptr_, sizeof(CpioHeader));

			auto magic = parseHex(hdr.magic, 6);
			assert(magic == 0x070701 || magic == 0x070702);

			auto nameSize = parseHex(hdr.nameSize, 8);
			auto fileSize = parseHex(hdr.fileSize, 8);

			ptr_ += ((sizeof(CpioHeader) + nameSize + 3) & ~uint32_t{3}) +
			        ((fileSize + 3) & ~uint32_t{3});
		}

		uint8_t *ptr_;
	};

	CpioRange(void *data) : data_{data} {}

	Iterator begin() { return Iterator{data_}; }

	Iterator end() {
		Iterator it{data_};

		// Seek to the end of the archive (we don't know the archive size)
		while ((*it).name != "TRAILER!!!")
			++it;

		return it;
	}

	void *eof() {
		auto it = end();
		return (++it).get();
	}

  private:
	void *data_;
};
