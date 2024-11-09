#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/ints.hpp>
#include <arch/variable.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {


void poisonPhysicalAccess(PhysicalAddr physical) { assert(!"Not implemented"); }
void poisonPhysicalWriteAccess(PhysicalAddr physical) { assert(!"Not implemented"); }


PageContext::PageContext()
: _nextStamp{1}, _primaryBinding{nullptr} { assert(!"Not implemented"); }


PageBinding::PageBinding()
: _asid{0}, _boundSpace{nullptr}, _primaryStamp{0}, _alreadyShotSequence{0} { assert(!"Not implemented"); }
bool PageBinding::isPrimary() { assert(!"Not implemented"); }
void PageBinding::rebind() { assert(!"Not implemented"); }
void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) { assert(!"Not implemented"); (void)space; }
void PageBinding::unbind() { assert(!"Not implemented"); }
void PageBinding::shootdown() { assert(!"Not implemented"); }


GlobalPageBinding::GlobalPageBinding()
: _alreadyShotSequence{0} { assert(!"Not implemented"); }
void GlobalPageBinding::bind() { assert(!"Not implemented"); }
void GlobalPageBinding::shootdown() { assert(!"Not implemented"); }


PageSpace::PageSpace(PhysicalAddr root_table)
: _rootTable{0}, _numBindings{0}, _shootSequence{0} { assert(!"Not implemented"); }
PageSpace::~PageSpace() { assert(!"Not implemented");}
void PageSpace::retire(RetireNode *node) { assert(!"Not implemented"); }
bool PageSpace::submitShootdown(ShootNode *node) { assert(!"Not implemented"); }
void PageSpace::activate(smarter::shared_ptr<PageSpace> space) { assert(!"Not implemented"); }


frg::manual_box<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize() {
	kernelSpaceSingleton.initialize(0);
}
KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpaceSingleton;
}

KernelPageSpace::KernelPageSpace(PhysicalAddr satp) { assert(!"Not implemented"); }
void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
	uint32_t flags, CachingMode caching_mode) { assert(!"Not implemented"); }
PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) { assert(!"Not implemented"); }
bool KernelPageSpace::submitShootdown(ShootNode *node) { assert(!"Not implemented"); }


ClientPageSpace::ClientPageSpace()
: PageSpace{physicalAllocator->allocate(kPageSize)} { assert(!"Not implemented"); }
ClientPageSpace::~ClientPageSpace() { assert(!"Not implemented");}
void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
	bool user_access, uint32_t flags, CachingMode caching_mode) { assert(!"Not implemented"); }
PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) { assert(!"Not implemented"); }
PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) { assert(!"Not implemented"); }
bool ClientPageSpace::isMapped(VirtualAddr pointer) { assert(!"Not implemented"); }
bool ClientPageSpace::updatePageAccess(VirtualAddr pointer) { assert(!"Not implemented"); }


ClientPageSpace::Walk::Walk(ClientPageSpace *space)
: _space{nullptr}, _accessor1{}, _accessor2{}, _accessor3{} { assert(!"Not implemented"); }
void ClientPageSpace::Walk::walkTo(uintptr_t address) { assert(!"Not implemented"); }
PageFlags ClientPageSpace::Walk::peekFlags() { assert(!"Not implemented"); }
PhysicalAddr ClientPageSpace::Walk::peekPhysical() { assert(!"Not implemented"); }
void ClientPageSpace::Walk::_update() { assert(!"Not implemented"); }

}
