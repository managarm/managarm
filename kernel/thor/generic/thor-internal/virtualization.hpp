#pragma once

#include <hel.h>

#include <thor-internal/address-space.hpp>
#include <thor-internal/error.hpp>

namespace thor {
	struct GuestState {
		uint64_t rax;
		uint64_t rbx;
		uint64_t rcx;
		uint64_t rdx;
		uint64_t rsi;
		uint64_t rdi;
		uint64_t rbp;
		uint64_t r8;
		uint64_t r9;
		uint64_t r10;
		uint64_t r11;
		uint64_t r12;
		uint64_t r13;
		uint64_t r14;
		uint64_t r15;
	} __attribute__((packed));

	struct VirtualizedCpu {
			virtual HelVmexitReason run() = 0;
			virtual void storeRegs(const HelX86VirtualizationRegs *regs) = 0;
			virtual void loadRegs(HelX86VirtualizationRegs *res) = 0;

	protected:
		~VirtualizedCpu() = default;
	};

	struct VirtualizedPageSpace : VirtualSpace {
		virtual Error store(uintptr_t guestAddress, size_t len, const void* buffer) = 0;
		virtual Error load(uintptr_t guestAddress, size_t len, void* buffer) = 0;
		virtual bool isMapped(VirtualAddr pointer) = 0;
		virtual bool submitShootdown(ShootNode *node) = 0;
		virtual void retire(RetireNode *node) = 0;

		virtual Error map(uint64_t guestAddress, uint64_t hostAddress, int flags) = 0;
		virtual PageStatus unmap(uint64_t guestAddress) = 0;
		VirtualizedPageSpace() : VirtualSpace{&ops_}, ops_{this} {}

		struct Operations final : VirtualOperations {
			Operations(VirtualizedPageSpace *space)
			: space_{space} { }

			void retire(RetireNode *node) override {
				return space_->retire(node);
			}

			bool submitShootdown(ShootNode *node) override {
				return space_->submitShootdown(node);
			}

			void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
					uint32_t flags, CachingMode) override {
				space_->map(pointer, physical, flags);
			}

			PageStatus unmapSingle4k(VirtualAddr pointer) override {
				return space_->unmap(pointer);
			}

			PageStatus cleanSingle4k(VirtualAddr) override {
				infoLogger() << "\e[31m" "thor: VirtualizedPageSpace::cleanSingle4k()"
						" is not properly supported" "\e[39m" << frg::endlog;
				return 0;
			}

			bool isMapped(VirtualAddr pointer) override {
				return space_->isMapped(pointer);
			}
			private:
				VirtualizedPageSpace *space_;
		};
	protected:
		~VirtualizedPageSpace() = default;
	private:
		Operations ops_;
	};
}
