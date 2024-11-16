#pragma once

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/virtualization.hpp>

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

		Error store(uintptr_t guestAddress, size_t len, const void *buffer);
		Error load(uintptr_t guestAddress, size_t len, void *buffer);

		Error map(uint64_t guestAddress, uint64_t hostAddress, int flags);
		PageStatus unmap(uint64_t guestAddress);
		bool isMapped(VirtualAddr pointer);

	private:
		uintptr_t translate(uintptr_t guestAddress);
		PhysicalAddr spaceRoot;
		frg::ticket_spinlock _mutex;
	};
} // namespace thor::svm
