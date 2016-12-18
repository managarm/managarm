
#include <string.h>

#include "common.hpp"
#include "process.hpp"

SharedProcess fork(SharedProcess parent) {
	auto data = std::make_shared<SharedProcess::Data>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	data->universe = helix::UniqueDescriptor(universe);

	HelHandle space;
	HEL_CHECK(helForkSpace(parent._data->space.getHandle(), &space));
	data->space = helix::UniqueDescriptor(space);

	HelHandle file_table_memory;
	void *file_table_window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
	HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite, &file_table_window));
	HEL_CHECK(helMapMemory(file_table_memory, data->space.getHandle(), nullptr,
			0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&data->clientFileTable));
	HEL_CHECK(helCloseDescriptor(file_table_memory));
	data->fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

	for(auto entry : parent._data->fileTable) {
		auto lane = getPassthroughLane(entry.second);

		HelHandle handle;
		HEL_CHECK(helTransferDescriptor(lane.getHandle(), universe, &handle));
		data->fileTableWindow[entry.first] = handle;
	}

	return SharedProcess(std::move(data));
}

