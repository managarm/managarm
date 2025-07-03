#include <array>
#include <core/smbios.hpp>
#include <ranges>

size_t getSmbiosEntrySize(frg::span<uint8_t> smbiosTable, size_t offset) {
	uint8_t len = smbiosTable[offset + 1];

	std::ranges::subrange remainingTable{smbiosTable.begin() + offset + len, smbiosTable.end()};
	std::array<uint8_t, 2> doubleZero = {0, 0};
	auto match = std::ranges::search(remainingTable, doubleZero);
	if(match.empty())
		return 0;
	return std::distance(smbiosTable.begin() + offset, match.end());
}

frg::span<uint8_t> getSmbiosEntry(frg::span<uint8_t> smbiosTable, uint8_t type) {
	size_t off = 0;

	while(off + 6 <= smbiosTable.size()) {
		uint8_t entryType = smbiosTable[off];
		size_t entrySize = getSmbiosEntrySize(smbiosTable, off);
		if(entryType == type)
			return {&smbiosTable[off], entrySize};

		off += entrySize;
	}

	return {};
}
