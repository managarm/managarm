#pragma once

#include <arch/register.hpp>
#include <arch/variable.hpp>

namespace spec {

namespace format {
inline constexpr uint32_t bgrx = 2;
inline constexpr uint32_t xrgb = 4;
} // namespace format

namespace cmd {
/* 2D commands */
inline constexpr uint32_t getDisplayInfo = 0x100;
inline constexpr uint32_t create2d = 0x101;
inline constexpr uint32_t resourceUnref = 0x102;
inline constexpr uint32_t setScanout = 0x103;
inline constexpr uint32_t resourceFlush = 0x104;
inline constexpr uint32_t xferToHost2d = 0x105;
inline constexpr uint32_t attachBacking = 0x106;
inline constexpr uint32_t detachBacking = 0x107;
inline constexpr uint32_t getCapsetInfo = 0x108;
inline constexpr uint32_t getCapset = 0x109;
inline constexpr uint32_t getEdid = 0x10A;
inline constexpr uint32_t assignUuid = 0x10B;
inline constexpr uint32_t createBlob = 0x10C;
inline constexpr uint32_t setScanoutBlob = 0x10D;
/* 3D commands */
inline constexpr uint32_t ctxCreate = 0x200;
inline constexpr uint32_t ctxDestroy = 0x201;
inline constexpr uint32_t ctxAttachResource = 0x202;
inline constexpr uint32_t ctxDetachResource = 0x203;
inline constexpr uint32_t create3d = 0x204;
inline constexpr uint32_t transferToHost3d = 0x205;
inline constexpr uint32_t transferFromHost3d = 0x206;
inline constexpr uint32_t submit3d = 0x207;
inline constexpr uint32_t resourceMapBlob = 0x208;
inline constexpr uint32_t resourceUnmapBlob = 0x209;
/* cursor commands */
inline constexpr uint32_t updateCursor = 0x300;
inline constexpr uint32_t moveCursor = 0x301;

} // namespace cmd

namespace resp {
/* success responses */
inline constexpr uint32_t noData = 0x1100;
inline constexpr uint32_t displayInfo = 0x1101;
inline constexpr uint32_t capsetInfo = 0x1102;
inline constexpr uint32_t capset = 0x1103;
inline constexpr uint32_t edid = 0x1104;
inline constexpr uint32_t resourceUuid = 0x1105;
inline constexpr uint32_t mapInfo = 0x1106;
/* error responses */
inline constexpr uint32_t unspec = 0x1200;
inline constexpr uint32_t outOfMemory = 0x1201;
inline constexpr uint32_t invalidScanout = 0x1202;
inline constexpr uint32_t invalidResource = 0x1203;
inline constexpr uint32_t invalidContext = 0x1204;
inline constexpr uint32_t invalidParameter = 0x1205;
} // namespace resp

struct Header {
	uint32_t type;
	uint32_t flags;
	uint64_t fenceId;
	uint32_t contextId;
	uint32_t padding;
};

struct Rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct DisplayInfo {
	Header header;
	struct {
		Rect rect;
		uint32_t enabled;
		uint32_t flags;
	} modes[16];
};

struct Create2d {
	Header header;
	uint32_t resourceId;
	uint32_t format;
	uint32_t width;
	uint32_t height;
};

struct AttachBacking {
	Header header;
	uint32_t resourceId;
	uint32_t numEntries;
};

struct MemEntry {
	uint64_t address;
	uint32_t length;
	uint32_t padding;
};

struct XferToHost2d {
	Header header;
	Rect rect;
	uint64_t offset;
	uint32_t resourceId;
	uint32_t padding;
};

struct SetScanout {
	Header header;
	Rect rect;
	uint32_t scanoutId;
	uint32_t resourceId;
};

struct ResourceFlush {
	Header header;
	Rect rect;
	uint32_t resourceId;
	uint32_t padding;
};

namespace cfg {
inline constexpr arch::scalar_register<uint32_t> numScanouts(8);
} // namespace cfg

} // namespace spec
