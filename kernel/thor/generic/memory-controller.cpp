#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/memory-controller.hpp>

namespace thor {

std::expected<frg::unique_ptr<MemoryController, KernelAlloc>, Error> MemoryController::create(
		MemoryController *parent, size_t floor) {
	frg::unique_ptr<MemoryController, KernelAlloc> ptr{*kernelAlloc,
			frg::construct<MemoryController>(*kernelAlloc, CtorToken{})};
	if(!ptr)
		return std::unexpected{Error::noMemory};
	if(parent)
		floor = frg::max(floor, parent->floor_);
	ptr->floor_ = floor;
	return ptr;
}

} // namespace thor
