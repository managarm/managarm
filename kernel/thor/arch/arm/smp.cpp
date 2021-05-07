#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/initgraph.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

extern "C" uint8_t _binary_kernel_thor_arch_arm_trampoline_bin_start[];
extern "C" uint8_t _binary_kernel_thor_arch_arm_trampoline_bin_end[];

namespace {
	struct StatusBlock {
		StatusBlock *self; // Pointer to this struct in the higher half.
		int targetStage;
		int cpuId;
		uintptr_t ttbr0;
		uintptr_t ttbr1;
		uintptr_t stack;
		void (*main)(StatusBlock *);
		CpuData *cpuContext;
	};

	struct Psci {
		Psci(DeviceTreeNode *node) {
			cpuOn_ = node->cpuOn();
			usesHvc_ = node->method() == "hvc";

			if (!usesHvc_)
				assert(node->method() == "smc");
		}

		int turnOnCpu(uint8_t id, uintptr_t addr) {
			register int64_t regResult asm("x0");
			register uint64_t regCmd asm("x0") = cpuOn_;
			register uint64_t regCpu asm("x1") = id;
			register uint64_t regAddr asm("x2") = addr;
			if (usesHvc_) {
				asm volatile ("hvc #0" : "=r"(regResult) : "r"(regCmd), "r"(regCpu), "r"(regAddr));
			} else {
				asm volatile ("smc #0" : "=r"(regResult) : "r"(regCmd), "r"(regCpu), "r"(regAddr));
			}

			return regResult;
		}

	private:
		uint32_t cpuOn_;
		bool usesHvc_;
	};

	frg::manual_box<Psci> psci_;

	void secondaryMain(StatusBlock *statusBlock) {
		initializeIrqVectors();

		auto cpuContext = statusBlock->cpuContext;
		setupCpuContext(cpuContext);

		initializeThisProcessor();

		cpuContext->archCpuIndex = statusBlock->cpuId;

		initGicOnThisCpu();
		initTimerOnThisCpu();

		__atomic_store_n(&statusBlock->targetStage, 2, __ATOMIC_RELEASE);

		infoLogger() << "Hello world on CPU #" << getCpuData()->cpuIndex << frg::endlog;

		Scheduler::resume(cpuContext->wqFiber);

		localScheduler()->update();
		localScheduler()->reschedule();
		localScheduler()->commitReschedule();

		while(1);
	}
}

void bootSecondary(DeviceTreeNode *node) {
	infoLogger() << "thor: Starting CPU \"" << node->path() << "\"" << frg::endlog;

	using EnableMethod = DeviceTreeNode::EnableMethod;

	if (node->enableMethod() == EnableMethod::unknown) {
		infoLogger() << "thor: We don't know how to start this CPU" << frg::endlog;
		return;
	}

	// Allocate a stack for the initialization code.
	constexpr size_t stackSize = 0x10000;
	void *stackPtr = kernelAlloc->allocate(stackSize);

	auto context = frg::construct<CpuData>(*kernelAlloc);

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	{
		auto irqLock = frg::guard(&irqMutex());

		context->globalBinding.bind();
	}

	auto codePhysPtr = physicalAllocator->allocate(kPageSize);
	PageAccessor codeAccessor{codePhysPtr};

	// We use a ClientPageSpace here to create an identity mapping for the trampoline
	ClientPageSpace lowMapping;
	lowMapping.mapSingle4k(codePhysPtr, codePhysPtr, false, page_access::execute, CachingMode::null);

	// Copy the trampoline code into the new stack
	auto imageSize = (uintptr_t)_binary_kernel_thor_arch_arm_trampoline_bin_end
			- (uintptr_t)_binary_kernel_thor_arch_arm_trampoline_bin_start;
	assert(imageSize <= kPageSize);

	memcpy(codeAccessor.get(), _binary_kernel_thor_arch_arm_trampoline_bin_start, imageSize);

	// Setup a status block to communicate information to the AP.
	auto statusBlock = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(codeAccessor.get())
			+ (kPageSize - sizeof(StatusBlock)));

	statusBlock->self = statusBlock;
	statusBlock->targetStage = 0;
	statusBlock->ttbr0 = lowMapping.rootTable();
	statusBlock->ttbr1 = KernelPageSpace::global().rootTable();
	statusBlock->stack = (uintptr_t)stackPtr + stackSize;
	statusBlock->main = &secondaryMain;
	statusBlock->cpuContext = context;

	uint8_t id = node->reg()[0].addr;
	statusBlock->cpuId = id;

	// TODO: We assume CPU 0 is the boot CPU, but potentially it could be some other one
	bool alreadyUp = (id == 0);
	bool dead = false;

	switch(node->enableMethod()) {
		case EnableMethod::spintable: {
			infoLogger() << "thor: This CPU uses a spin-table" << frg::endlog;

			if (!alreadyUp) {
				auto ptr = node->cpuReleaseAddr();

				infoLogger() << "thor: Release address is " << frg::hex_fmt{ptr} << frg::endlog;

				auto page = ptr & ~(kPageSize - 1);
				auto offset = ptr & (kPageSize - 1);

				PageAccessor releaseAccessor{page};

				auto space = arch::mem_space{releaseAccessor.get()};
				arch::scalar_store<uintptr_t>(space, offset, codePhysPtr);

				asm volatile ("sev" ::: "memory");
			}

			break;
		}

		case EnableMethod::psci: {
			infoLogger() << "thor: This CPU uses PSCI" << frg::endlog;
			if (!psci_) {
				infoLogger() << "thor: PSCI was not detected" << frg::endlog;
				return;
			}

			if (!alreadyUp) {
				int res = psci_->turnOnCpu(id, codePhysPtr);
				if (res < 0) {
					constexpr const char *errors[] = {
						"Success",
						"Not supported",
						"Invalid parameters",
						"Denied",
						"Already on",
						"On pending",
						"Internal failure",
						"Not present",
						"Disabled",
						"Invalid address"
					};
					infoLogger() << "thor: Booting AP failed with " << errors[-res] << frg::endlog;
					dead = true;
				}
			}

			break;
		}

		default:
			panicLogger() << "thor: Illegal enable method" << frg::endlog;
	}

	// Wait for AP to leave the stub so we can free it and the mapping it used
	if (!alreadyUp && !dead) {
		while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) == 0)
			;
	}

	physicalAllocator->free(codePhysPtr, kPageSize);

	if (dead || alreadyUp) {
		kernelAlloc->deallocate(stackPtr, stackSize);
	}
}

static initgraph::Task initAPs{&globalInitEngine, "arm.init-aps",
	initgraph::Requires{getDeviceTreeParsedStage(), getTaskingAvailableStage()},
	[] {
		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<1>({"arm,psci"})) {
				psci_.initialize(node);
				return true;
			}

			return false;
		});

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<2>({"arm,cortex-a72", "arm,cortex-a53"})) {
				bootSecondary(node);
			}

			return false;
		});
	}
};
}
