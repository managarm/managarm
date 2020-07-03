#pragma once

#include <arch/x86/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/virtualization.hpp>

constexpr uint64_t EPT_READ = (0);
constexpr uint64_t EPT_WRITE = (1);
constexpr uint64_t EPT_EXEC = (2);
constexpr uint64_t EPT_USEREXEC = (10);
constexpr uint64_t EPT_PHYSADDR = (12);
constexpr uint64_t EPT_IGNORE_PAT = (6);
constexpr uint64_t EPT_MEMORY_TYPE = (3);

namespace thor::vmx {

struct EptPtr {
	uint64_t eptp;
	uint64_t gpa;
};

struct EptSpace : VirtualizedPageSpace {
	friend struct Vmcs;
	friend struct ShootNode;

	EptSpace(PhysicalAddr root) : spaceRoot(root){}
	~EptSpace();
	EptSpace(const EptSpace &ept2) = delete;
	EptSpace& operator=(const EptSpace &ept2) = delete;
	bool submitShootdown(ShootNode *node);
	void retire(RetireNode *node);

	static smarter::shared_ptr<EptSpace> create(size_t root) {
		auto ptr = smarter::allocate_shared<EptSpace>(Allocator{}, root);
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0, 0x7ffffff00000);
		return ptr;
	}

	Error store(uintptr_t guestAddress, size_t len, const void* buffer);
	Error load(uintptr_t guestAddress, size_t len, void* buffer);

	Error map(uint64_t guestAddress, uint64_t hostAddress, int flags);
	PageStatus unmap(uint64_t guestAddress);
	bool isMapped(VirtualAddr pointer);

private:
	uintptr_t translate(uintptr_t guestAddress);
	PhysicalAddr spaceRoot;
	frigg::TicketLock _mutex;
};

} // namespace thor
