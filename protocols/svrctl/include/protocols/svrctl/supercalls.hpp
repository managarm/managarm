#pragma once

#include <hel.h>
#include <stdint.h>

namespace protocols {
namespace svrctl {

inline constexpr uint32_t superGetServerData = 64;

struct ManagarmServerData {
	HelHandle hardwareAccess;
	HelHandle controlLane;
};

} } // namespace protocols::svrctl
