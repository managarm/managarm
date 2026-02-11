#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>
#include <initgraph.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <thor-internal/rcu.hpp>

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
			auto methodProp = node->dtNode().findProperty("method");
			if (!methodProp)
				panicLogger() << node->path() << " has no method" << frg::endlog;
			auto method = methodProp->asString();

			usesHvc_ = method == "hvc";
			if (!usesHvc_)
				assert(method == "smc");

			auto onProp = node->dtNode().findProperty("cpu-on");
			if (!onProp) {
				cpuOn_ = 0xC4000003;
			} else {
				auto it = onProp->access();
				if (!it.readCells(cpuOn_, 1))
					panicLogger()
						<< node->path() << ": failed to read cpu-on" << frg::endlog;
			}
		}

		int turnOnCpu(uint64_t id, uintptr_t addr) {
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

		LoadBalancer::singleton().setOnline(cpuContext);
		setRcuOnline(cpuContext);
		auto *scheduler = &localScheduler.get();
		scheduler->update();
		scheduler->forceReschedule();
		scheduler->commitReschedule();

		while(1);
	}
}

bool bootSecondary(DeviceTreeNode *node, size_t cpuIndex) {
	infoLogger() << "thor: Starting CPU \"" << node->path() << "\"" << frg::endlog;
	uint64_t id = node->reg()[0].addr;

	auto methodProp = node->dtNode().findProperty("enable-method");
	if (!methodProp)
		panicLogger() << node->path() << " has no enable-method" << frg::endlog;

	enum class EnableMethod {
		unknown,
		spinTable,
		psci
	} method = EnableMethod::unknown;

	size_t i = 0;
	while (i < methodProp->size()) {
		frg::string_view sv{reinterpret_cast<const char *>(methodProp->data()) + i};
		i += sv.size() + 1;

		if (sv == "psci")
			method = EnableMethod::psci;
		else if (sv == "spin-table")
			method = EnableMethod::spinTable;
	}

	if (method == EnableMethod::unknown) {
		infoLogger() << "thor: We don't know how to start this CPU" << frg::endlog;
		return false;
	}

	// Allocate a stack for the initialization code.
	constexpr size_t stackSize = 0x10000;
	void *stackPtr = kernelAlloc->allocate(stackSize);

	auto *context = getCpuData(cpuIndex);
	context->localLogRing = frg::construct<ReentrantRecordRing>(*kernelAlloc);

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	initializeAsidContext(context);

	auto codePhysPtr = physicalAllocator->allocate(kPageSize);
	auto codeVirtPtr = KernelVirtualMemory::global().allocate(kPageSize);

	KernelPageSpace::global().mapSingle4k(VirtualAddr(codeVirtPtr), codePhysPtr,
			page_access::write, CachingMode::uncached);

	// We use a ClientPageSpace here to create an identity mapping for the trampoline
	ClientPageSpace lowMapping;
	ClientPageSpace::Cursor cursor{&lowMapping, codePhysPtr};
	cursor.map4k(codePhysPtr, page_access::execute, CachingMode::null);
	*cursor.getPtePtr() &= ~kPagePXN; // Workaround: clear PXN so the AP can execute code from the page.

	auto imageSize = (uintptr_t)_binary_kernel_thor_arch_arm_trampoline_bin_end
			- (uintptr_t)_binary_kernel_thor_arch_arm_trampoline_bin_start;
	assert(imageSize <= kPageSize);

	memcpy(codeVirtPtr, _binary_kernel_thor_arch_arm_trampoline_bin_start, imageSize);

	// Setup a status block to communicate information to the AP.
	auto statusBlock = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(codeVirtPtr)
			+ (kPageSize - sizeof(StatusBlock)));

	statusBlock->self = statusBlock;
	statusBlock->targetStage = 0;
	statusBlock->ttbr0 = lowMapping.rootTable();
	statusBlock->ttbr1 = KernelPageSpace::global().rootTable();
	statusBlock->stack = (uintptr_t)stackPtr + stackSize;
	statusBlock->main = &secondaryMain;
	statusBlock->cpuContext = context;
	statusBlock->cpuId = id;

	bool dontWait = false;

	switch(method) {
		case EnableMethod::spinTable: {
			infoLogger() << "thor: This CPU uses a spin-table" << frg::endlog;


			auto addrProp = node->dtNode().findProperty("cpu-release-addr");
			if (!addrProp)
				panicLogger() << node->path() << " has no cpu-release-addr" << frg::endlog;

			uint64_t ptr;
			auto it = addrProp->access();
			if(!it.readCells(ptr, 2)) {
				if(!it.readCells(ptr, 1)) {
					panicLogger() << node->path() << " has an empty cpu-release-addr" << frg::endlog;
				}
			}

			infoLogger() << "thor: Release address is " << frg::hex_fmt{ptr} << frg::endlog;

			auto page = ptr & ~(kPageSize - 1);
			auto offset = ptr & (kPageSize - 1);

			auto virtPtr = KernelVirtualMemory::global().allocate(kPageSize);

			KernelPageSpace::global().mapSingle4k(VirtualAddr(virtPtr), page,
					page_access::write, CachingMode::uncached);

			auto space = arch::mem_space{virtPtr};

			arch::scalar_store<uintptr_t>(space, offset, codePhysPtr);

			asm volatile ("sev" ::: "memory");

			KernelPageSpace::global().unmapSingle4k(VirtualAddr(virtPtr));

			KernelVirtualMemory::global().deallocate(virtPtr, kPageSize);

			break;
		}

		case EnableMethod::psci: {
			infoLogger() << "thor: This CPU uses PSCI" << frg::endlog;
			if (!psci_) {
				infoLogger() << "thor: PSCI was not detected" << frg::endlog;
				return false;
			}

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
				dontWait = true;
			}

			break;
		}

		default:
			panicLogger() << "thor: Illegal enable method" << frg::endlog;
	}

	// Wait for AP to leave the stub so we can free it and the mapping it used
	if (!dontWait) {
		while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) == 0)
			;
	}

	KernelPageSpace::global().unmapSingle4k(VirtualAddr(codeVirtPtr));
	KernelVirtualMemory::global().deallocate(codeVirtPtr, kPageSize);
	KernelFiber::asyncBlockCurrent(
		shootdown(
			&KernelPageSpace::global(),
			VirtualAddr(codeVirtPtr),
			kPageSize,
			WorkQueue::generalQueue().get()
		)
	);
	physicalAllocator->free(codePhysPtr, kPageSize);

	if (dontWait) {
		kernelAlloc->deallocate(stackPtr, stackSize);
	}

	return !dontWait;
}

static initgraph::Task initAPs{&globalInitEngine, "arm.init-aps",
	initgraph::Requires{getDeviceTreeParsedStage(), getTaskingAvailableStage()},
	[] {
		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<2>({"arm,psci", "arm,psci-1.0"})) {
				psci_.initialize(node);
				return true;
			}

			return false;
		});

		auto bspAffinity = getCpuData()->affinity;

		size_t apCpuIndex = 1;
		auto bootApFromDt = [&](DeviceTreeNode *node) {
			if (!node->isCompatible<4>({"arm,cortex-a72", "arm,cortex-a53", "arm,arm-v8", "arm,armv8"}))
				return;

			auto affinity = node->reg()[0].addr;
			if (affinity == bspAffinity)
				return;

			if (static_cast<uint64_t>(apCpuIndex) >= cpuConfigNote->totalCpus) {
				panicLogger() << "thor: CPU index " << apCpuIndex
						<< " exceeds expected number of CPUs " << cpuConfigNote->totalCpus
						<< frg::endlog;
			}

			bootSecondary(node, apCpuIndex);
			++apCpuIndex;
		};

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			bootApFromDt(node);
			return false;
		});

		if (getCpuCount() != cpuConfigNote->totalCpus)
			panicLogger() << "thor: Booted " << getCpuCount()
					<< " CPUs but Eir detected " << cpuConfigNote->totalCpus
					<< frg::endlog;
	}
};
}
