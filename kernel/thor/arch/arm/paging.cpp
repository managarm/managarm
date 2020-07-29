#include <thor-internal/arch/paging.hpp>

namespace thor {

void poisonPhysicalAccess(PhysicalAddr physical) {}
void poisonPhysicalWriteAccess(PhysicalAddr physical) {}

PageContext::PageContext() {}

PageBinding::PageBinding() {}
bool PageBinding::isPrimary() { return false; }
void PageBinding::rebind() {}
void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {}
void PageBinding::unbind() {}
void PageBinding::shootdown() {}

GlobalPageBinding::GlobalPageBinding() {}
void GlobalPageBinding::bind() {}
void GlobalPageBinding::shootdown() {}

void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {}
PageSpace::PageSpace(PhysicalAddr root_table) {}
PageSpace::~PageSpace() {}
void PageSpace::retire(RetireNode *node) {}
bool PageSpace::submitShootdown(ShootNode *node) { return false; }

void KernelPageSpace::initialize(PhysicalAddr pml4_address) {}
KernelPageSpace &KernelPageSpace::global() {}
KernelPageSpace::KernelPageSpace(PhysicalAddr pml4_address) {}
bool KernelPageSpace::submitShootdown(ShootNode *node) { return false; }
void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		uint32_t flags, CachingMode caching_mode) {}
PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {}

ClientPageSpace::Walk::Walk(ClientPageSpace *space) {}
ClientPageSpace::Walk::~Walk() {}
void ClientPageSpace::Walk::walkTo(uintptr_t address) {}
PageFlags ClientPageSpace::Walk::peekFlags() {}
PhysicalAddr ClientPageSpace::Walk::peekPhysical() {}

ClientPageSpace::ClientPageSpace() :PageSpace{0} {}
ClientPageSpace::~ClientPageSpace() {}

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access,
		uint32_t flags, CachingMode caching_mode) {}
PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) {}
PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) {}
void ClientPageSpace::unmapRange(VirtualAddr pointer, size_t size, PageMode mode) {}
bool ClientPageSpace::isMapped(VirtualAddr pointer) {}

void initializePhysicalAccess() {}

}
