#pragma once

#include <stdint.h>

namespace posix {

inline constexpr uint32_t superGetProcessData = 1;
inline constexpr uint32_t superFork = 2;
inline constexpr uint32_t superExecve = 3;
inline constexpr uint32_t superExit = 4;
inline constexpr uint32_t superSigKill = 5;
inline constexpr uint32_t superSigRestore = 6;
inline constexpr uint32_t superSigMask = 7;
inline constexpr uint32_t superSigRaise = 8;
inline constexpr uint32_t superClone = 9;
inline constexpr uint32_t superAnonAllocate = 10;
inline constexpr uint32_t superAnonDeallocate = 11;
inline constexpr uint32_t superSigAltStack = 12;
inline constexpr uint32_t superSigSuspend = 13;
inline constexpr uint32_t superGetTid = 14;
inline constexpr uint32_t superSigGetPending = 15;
inline constexpr uint32_t superGetServerData = 64;

} // namespace posix
