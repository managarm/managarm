#pragma once

#include <expected>
#include <frg/unique.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

struct MemoryController {
private:
	struct CtorToken {};

public:
	// parent is null for the root controller.
	// The floor (in pages) is raised to the floor of the parent.
	static std::expected<frg::unique_ptr<MemoryController, KernelAlloc>, Error> create(
			MemoryController *parent, size_t floor);

	MemoryController(CtorToken) { }

	MemoryController(const MemoryController &) = delete;

	MemoryController &operator= (const MemoryController &) = delete;

	// Pages that user-class allocations of hierarchies using this controller leave free on top
	// of the user floor; see AllocationPolicy::floor.
	size_t floor() const {
		return floor_;
	}

private:
	size_t floor_{0};
};

} // namespace thor
