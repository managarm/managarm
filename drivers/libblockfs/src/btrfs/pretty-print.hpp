#pragma once

#include <arch/bit.hpp>
#include <format>
#include <utility>

#include "spec.hpp"

template <>
struct std::formatter<blockfs::btrfs::UUID> : std::formatter<std::string> {
	auto format(const blockfs::btrfs::UUID &uuid, auto &ctx) const {
		using arch::convert_endian;
		using arch::endian;

		return std::format_to(
		    ctx.out(),
		    "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		    convert_endian<endian::big>(uuid.data1),
		    convert_endian<endian::big>(uuid.data2),
		    convert_endian<endian::big>(uuid.data3),
		    uuid.data4[0],
		    uuid.data4[1],
		    uuid.data4[2],
		    uuid.data4[3],
		    uuid.data4[4],
		    uuid.data4[5],
		    uuid.data4[6],
		    uuid.data4[7]
		);
	}
};

template <>
struct std::formatter<blockfs::btrfs::ItemType> : std::formatter<std::string> {
	auto format(const blockfs::btrfs::ItemType &type, auto &ctx) const {
		switch (type) {
			case blockfs::btrfs::ItemType::INODE_ITEM:
				return std::format_to(ctx.out(), "INODE_ITEM");
			case blockfs::btrfs::ItemType::INODE_REF:
				return std::format_to(ctx.out(), "INODE_REF");
			case blockfs::btrfs::ItemType::XATTR_ITEM:
				return std::format_to(ctx.out(), "XATTR_ITEM");
			case blockfs::btrfs::ItemType::DIR_ITEM:
				return std::format_to(ctx.out(), "DIR_ITEM");
			case blockfs::btrfs::ItemType::DIR_INDEX:
				return std::format_to(ctx.out(), "DIR_INDEX");
			case blockfs::btrfs::ItemType::EXTENTDATA_ITEM:
				return std::format_to(ctx.out(), "EXTENTDATA_ITEM");
			case blockfs::btrfs::ItemType::EXTENT_CSUM:
				return std::format_to(ctx.out(), "EXTENT_CSUM");
			case blockfs::btrfs::ItemType::ROOT_ITEM:
				return std::format_to(ctx.out(), "ROOT_ITEM");
			case blockfs::btrfs::ItemType::EXTENT_ITEM:
				return std::format_to(ctx.out(), "EXTENT_ITEM");
			case blockfs::btrfs::ItemType::BLOCK_GROUP_ITEM:
				return std::format_to(ctx.out(), "BLOCK_GROUP_ITEM");
			case blockfs::btrfs::ItemType::FREE_SPACE_INFO:
				return std::format_to(ctx.out(), "FREE_SPACE_INFO");
			case blockfs::btrfs::ItemType::FREE_SPACE_EXTENT:
				return std::format_to(ctx.out(), "FREE_SPACE_EXTENT");
			case blockfs::btrfs::ItemType::DEV_EXTENT_ITEM:
				return std::format_to(ctx.out(), "DEV_EXTENT_ITEM");
			case blockfs::btrfs::ItemType::DEV_ITEM:
				return std::format_to(ctx.out(), "DEV_ITEM");
			case blockfs::btrfs::ItemType::CHUNK_ITEM:
				return std::format_to(ctx.out(), "CHUNK_ITEM");
			case blockfs::btrfs::ItemType::DEV_STATS_ITEM:
				return std::format_to(ctx.out(), "DEV_STATS_ITEM");
			default:
				return std::format_to(ctx.out(), "UNKNOWN({})", std::to_underlying(type));
		}
	}
};

template <>
struct std::formatter<blockfs::btrfs::key> : std::formatter<std::string> {
	auto format(const blockfs::btrfs::key &key, auto &ctx) const {
		return std::format_to(ctx.out(), "({}, {}, {:#x})", key.objectid, key.type, key.offset);
	}
};

template <>
struct std::formatter<blockfs::btrfs::LogicalAddress> : std::formatter<std::string> {
	auto format(const blockfs::btrfs::LogicalAddress &addr, auto &ctx) const {
		return std::format_to(ctx.out(), "LogicalAddress({:#x})", uint64_t(addr));
	}
};

template <>
struct std::formatter<blockfs::btrfs::PhysicalAddress> : std::formatter<std::string> {
	auto format(const blockfs::btrfs::PhysicalAddress &addr, auto &ctx) const {
		return std::format_to(ctx.out(), "PhysicalAddress({:#x})", uint64_t(addr));
	}
};
