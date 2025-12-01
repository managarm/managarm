#pragma once

#include <compare>
#include <stdint.h>

namespace blockfs::btrfs {

struct [[gnu::packed]] LogicalAddress {
	constexpr LogicalAddress() = default;
	explicit constexpr LogicalAddress(uint64_t addr) : addr_{addr} {}

	explicit constexpr operator uint64_t() const { return addr_; }

	auto operator<=>(const LogicalAddress &) const = default;

private:
	uint64_t addr_ = 0;
};

struct FileSystem;

struct [[gnu::packed]] PhysicalAddress {
	constexpr PhysicalAddress() = default;

	explicit constexpr PhysicalAddress(uint64_t addr) : addr_{addr} {}
	explicit PhysicalAddress(FileSystem *fs, LogicalAddress logicalAddr);

	explicit constexpr operator uint64_t() const { return addr_; }

	auto operator<=>(const PhysicalAddress &) const = default;

private:
	uint64_t addr_;
};

} // namespace blockfs::btrfs
