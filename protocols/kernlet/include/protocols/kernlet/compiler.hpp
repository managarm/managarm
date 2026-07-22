#pragma once

#include <async/result.hpp>
#include <fafnir/dsl.hpp>
#include <helix/ipc.hpp>
#include <vector>

enum class BindType {
	null,
	offset,
	memoryView,
	bitsetEvent
};

// Fafnir value types for the kernlet bindings, mirroring BindType.
namespace kernlet_types {
	struct memory_view_tag { };
	struct bitset_event_tag { };

	using offset = fnr::u32;
	using memory_view = fnr::opaque<memory_view_tag>;
	using bitset_event = fnr::opaque<bitset_event_tag>;
}

// Signatures of the kernel intrinsics; keep in sync with intrinsic_signature() in kernletcc.
namespace kernlet_intrin {
	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::offset>,
		fnr::types<fnr::u32>
	> pio_read16{"__pio_read16"};

	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::offset, fnr::u32>,
		fnr::types<>
	> pio_write16{"__pio_write16"};

	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::memory_view, kernlet_types::offset>,
		fnr::types<fnr::u32>
	> mmio_read8{"__mmio_read8"};

	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::memory_view, kernlet_types::offset>,
		fnr::types<fnr::u32>
	> mmio_read32{"__mmio_read32"};

	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::memory_view, kernlet_types::offset, fnr::u32>,
		fnr::types<>
	> mmio_write32{"__mmio_write32"};

	inline constexpr fnr::intrin<
		fnr::types<kernlet_types::bitset_event, fnr::u32>,
		fnr::types<>
	> trigger_bitset{"__trigger_bitset"};
}

async::result<void> connectKernletCompiler();
async::result<helix::UniqueDescriptor> compile(void *code, size_t size,
		std::vector<BindType> bind_types);
