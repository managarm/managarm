
#include <string.h>

#include "common.hpp"
#include "process.hpp"

SharedProcess fork(SharedProcess parent) {
	auto data = std::make_shared<SharedProcess::Data>();
	HelHandle space;
	HEL_CHECK(helForkSpace(parent._data->space.getHandle(), &space));
	data->space = helix::UniqueDescriptor(space);
	return SharedProcess(std::move(data));
}

