#pragma once

#include <arch/x86/paging.hpp>
#include <generic/error.hpp>
#include <generic/virtualization.hpp>

constexpr uint64_t EPT_READ = (0);
constexpr uint64_t EPT_WRITE = (1);
constexpr uint64_t EPT_EXEC = (2);
constexpr uint64_t EPT_USEREXEC = (10);
constexpr uint64_t EPT_PHYSADDR = (12);
constexpr uint64_t EPT_IGNORE_PAT = (6);
constexpr uint64_t EPT_MEMORY_TYPE = (3);

namespace thor::vmx {

struct EptSpace : VirtualizedPageSpace {
	friend class Vmcs;

	EptSpace(PhysicalAddr root) : spaceRoot(root){}
	~EptSpace();
	EptSpace(const EptSpace &ept2) = delete;
	EptSpace& operator=(const EptSpace &ept2) = delete;


	Error store(uintptr_t guestAddress, size_t len, const void* buffer);
	Error load(uintptr_t guestAddress, size_t len, void* buffer);

	void map(uint64_t guestAddress, int flags);

private:
	uintptr_t translate(uintptr_t guestAddress);
	PhysicalAddr spaceRoot;
	frigg::TicketLock _mutex;
};

} // namespace thor
