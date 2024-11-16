#pragma once

#include <thor-internal/arch/paging.hpp>

#include <thor-internal/arch-generic/cursor.hpp>

namespace thor {

template <typename T>
concept ValidCursor = requires (T t, uintptr_t va, PhysicalAddr pa,
		PageFlags flags, CachingMode mode) {
	{ t.virtualAddress() } -> std::same_as<uintptr_t>;
	{ t.moveTo(va) } -> std::same_as<void>;
	{ t.advance4k() } -> std::same_as<void>;
	{ t.findPresent(va) } -> std::same_as<bool>;
	{ t.findDirty(va) } -> std::same_as<bool>;
	{ t.map4k(pa, flags, mode) } -> std::same_as<void>;
	{ t.remap4k(pa, flags, mode) } -> std::same_as<PageStatus>;
	{ t.clean4k() } -> std::same_as<PageStatus>;
	{ t.unmap4k() } -> std::same_as<std::tuple<PageStatus, PhysicalAddr>>;
};

template <typename T>
concept ValidPageSpace =
	std::derived_from<T, PageSpace>
	&& ValidCursor<typename T::Cursor>;
static_assert(ValidPageSpace<KernelPageSpace>);
static_assert(ValidPageSpace<ClientPageSpace>);

template <typename T>
concept ValidKernelPageSpace = requires (T t, VirtualAddr va, PhysicalAddr pa,
		PageFlags flags, CachingMode mode) {
	{ T::global() } -> std::same_as<T &>;
	{ T::initialize() } -> std::same_as<void>;

	// TODO(qookie): Instead of requiring {un,}mapSingle4k (which
	// use cursors internally anyway), we could provide generic
	// {un,}map4kInSpace functions using cursors.
	{ t.mapSingle4k(va, pa, flags, mode) } -> std::same_as<void>;
	{ t.unmapSingle4k(va) } -> std::same_as<PhysicalAddr>;
};
static_assert(ValidKernelPageSpace<KernelPageSpace>);

template <typename T>
concept ValidClientPageSpace = requires (T t, VirtualAddr va) {
	// Used for dirty bit emulation on architectures that don't
	// have hardware dirty bit management.
	// Invoked on a page fault due to a write to read-only page.
	// Checks whether the given page is supposed to be writable,
	// and makes it so if that's the case. Returns whether this
	// page was modified (=> if true, this page fault requires no
	// further action).
	{ t.updatePageAccess(va) } -> std::same_as<bool>;
};
static_assert(ValidClientPageSpace<ClientPageSpace>);

} // namespace thor
