#pragma once

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::svm {

struct NptPageSpace : PageSpace {
	NptPageSpace(PhysicalAddr root);

	NptPageSpace(const NptPageSpace &) = delete;

	~NptPageSpace();

	NptPageSpace &operator= (const NptPageSpace &) = delete;
};

struct NptOperations final : VirtualOperations {
	NptOperations(NptPageSpace *pageSpace);

	void retire(RetireNode *node) override;

	bool submitShootdown(ShootNode *node) override;

	frg::expected<Error> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags) override;

	frg::expected<Error> remapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags) override;

	frg::expected<Error> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, PageFlags flags) override;

	frg::expected<Error> cleanPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size) override;

	frg::expected<Error> unmapPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size) override;

private:
	NptPageSpace *pageSpace_;
};

} // namespace thor::svm

namespace thor::svm {
	struct NptSpace final : VirtualizedPageSpace {
		friend struct ShootNode;
		friend struct Vcpu;

		NptSpace(PhysicalAddr root) : spaceRoot(root){}
		~NptSpace();
		NptSpace(const NptSpace &) = delete;
		NptSpace& operator=(const NptSpace &) = delete;
		bool submitShootdown(ShootNode *node);
		void retire(RetireNode *node);

		static smarter::shared_ptr<NptSpace> create(size_t root) {
			auto ptr = smarter::allocate_shared<NptSpace>(Allocator{}, root);
			ptr->selfPtr = ptr;
			ptr->setupInitialHole(0, 0x7ffffff00000);
			return ptr;
		}

		Error map(uint64_t guestAddress, uint64_t hostAddress, int flags);
		PageStatus unmap(uint64_t guestAddress);
		bool isMapped(VirtualAddr pointer);

	private:
		PhysicalAddr spaceRoot;
		frg::ticket_spinlock _mutex;
	};
} // namespace thor::svm
