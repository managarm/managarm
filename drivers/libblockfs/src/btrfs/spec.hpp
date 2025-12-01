#pragma once

#include <format>
#include <generator>
#include <stdint.h>
#include <utility>

#include "types.hpp"

namespace blockfs::btrfs {

struct [[gnu::packed]] timespec {
	int64_t sec;
	int32_t nsec;
};

struct [[gnu::packed]] UUID {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

enum class ItemType : uint8_t {
	INODE_ITEM = 0x01,
	INODE_REF = 0x0C,
	XATTR_ITEM = 0x18,
	DIR_ITEM = 0x54,
	DIR_INDEX = 0x60,
	EXTENTDATA_ITEM = 0x6C,
	EXTENT_CSUM = 0x80,
	ROOT_ITEM = 0x84,
	EXTENT_ITEM = 0xA0,
	BLOCK_GROUP_ITEM = 0xC0,
	FREE_SPACE_INFO = 0xC6,
	FREE_SPACE_EXTENT = 0xC7,
	DEV_EXTENT_ITEM = 0xCC,
	DEV_ITEM = 0xD8,
	CHUNK_ITEM = 0xE4,
	DEV_STATS_ITEM = 0xF9,
};

struct [[gnu::packed]] key {
	uint64_t objectid;
	ItemType type;
	uint64_t offset;

	auto operator<=>(const key&) const = default;

	// Return a key with zero offset.
	// This can be useful for comparing keys if you only care about objectid and type.
	key noOffset() const {
		return {objectid, type};
	}
};

static_assert(sizeof(key) == 0x11, "Bad key size");

struct [[gnu::packed]] key_ptr {
	key k;
	LogicalAddress addr;
	uint64_t generation;
};

struct [[gnu::packed]] dir_item {
	key location;
	uint64_t transaction_id;
	uint16_t data_len;
	uint16_t name_len;
	uint8_t type;
};

static_assert(sizeof(dir_item) == 30, "Bad dir_item size");

struct [[gnu::packed]] device_item {
	uint64_t device_id;
	uint64_t bytes;
	uint64_t bytes_used;
	uint32_t preferred_io_alignment;
	uint32_t preferred_io_width;
	uint32_t minimum_io_size;
	uint64_t type;
	uint64_t generation;
	uint64_t start_offset;
	uint32_t dev_group;
	uint8_t seek_speed;
	uint8_t bandwidth;
	UUID device_uuid;
	UUID fd_uuid;
};

struct [[gnu::packed]] chunk_item {
	uint64_t chunk_size;
	uint64_t object_id;
	uint64_t stripe_size;
	uint64_t type;
	uint32_t preferred_io_alignment;
	uint32_t preferred_io_width;
	uint32_t minimum_io_size;
	uint16_t stripe_count;
	uint16_t sub_stripes;
};

struct [[gnu::packed]] chunk_stripe {
	uint64_t device_id;
	PhysicalAddress offset;
	UUID device_uuid;
};

struct [[gnu::packed]] inode_item {
	uint64_t generation;
	uint64_t transaction_id;
	uint64_t size;
	uint64_t nbytes;
	uint64_t block_group;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t mode;
	uint64_t rdev;
	uint64_t flags;
	uint64_t sequence;
	uint64_t __reserved[4];
	timespec atime;
	timespec ctime;
	timespec mtime;
	timespec otime;
};

static_assert(sizeof(inode_item) == 160, "Bad inode_item size");

struct [[gnu::packed]] extent_data {
	uint64_t generation;
	uint64_t decoded_size;
	uint8_t compression;
	uint8_t encryption;
	uint16_t other_encoding;
	uint8_t type;
};

struct [[gnu::packed]] extent_data_extra {
	LogicalAddress extent_addr;
	uint64_t extent_size;
	uint64_t extent_offset;
	uint64_t num_bytes;
};

struct [[gnu::packed]] root_item {
	inode_item inode;
	uint64_t generation;
	uint64_t root_dir_id;
	uint64_t bytenr;
	uint64_t byte_limit;
	uint64_t bytes_used;
	uint64_t last_snapshot;
	uint64_t flags;
	uint32_t refs;
	uint8_t padding[219];
};

struct [[gnu::packed]] superblock {
	char csum[0x20];
	UUID fs_uuid;
	uint64_t physical_address;
	uint64_t flags;
	char magic[8];
	uint64_t generation;
	LogicalAddress root_tree_root;
	LogicalAddress chunk_tree_root;
	LogicalAddress log_tree_root;
	uint64_t log_root_transid;
	uint64_t total_bytes;
	uint64_t bytes_used;
	uint64_t root_dir_objectid;
	uint64_t num_devices;
	uint32_t sector_size;
	uint32_t node_size;
	uint32_t leaf_size;
	uint32_t stripe_size;
	uint32_t sys_chunk_array_size;
	uint64_t chunk_root_generation;
	uint64_t compat_flags;
	uint64_t compat_ro_flags;
	uint64_t inompat_flags;
	uint16_t checksum_type;
	uint8_t root_level;
	uint8_t chunk_root_level;
	uint8_t log_root_level;
	device_item dev_item_data;
	char label[0x100];
	uint64_t cache_generation;
	uint64_t uuid_tree_generation;
	char __padding[0xF0];
};

static_assert(
    sizeof(superblock) == 811, "The superblock size at the bootstrap chunks must be 824 bytes"
);

struct [[gnu::packed]] block_header {
	uint8_t csum[0x20];
	UUID fs_uuid;
	uint64_t bytenr;
	uint64_t flags;
	UUID chunk_tree_uuid;
	uint64_t generation;
	uint64_t owner;
	uint32_t nritems;
	uint8_t level;
};

static_assert(sizeof(block_header) == 101, "Bad block_header size");

struct [[gnu::packed]] item {
	key k;
	uint32_t data_offset;
	uint32_t data_size;
};

static_assert(sizeof(item) == 25, "Bad item size");

} // namespace blockfs::btrfs
