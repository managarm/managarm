#include <frg/optional.hpp>
#include <frg/scope_exit.hpp>
#include <frg/utility.hpp>
#include <stddef.h>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/cpu.hpp>
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
#include <thor-internal/rcu.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

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

	enum class EnableMethod {
		unknown,
		spinTable,
		psci
	};

	constexpr uint32_t psciIdCpuOn64 = 0xC4000003;

	struct Psci {
		Psci(bool usesHvc) : cpuOn_{psciIdCpuOn64}, usesHvc_{usesHvc} {}

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
				cpuOn_ = psciIdCpuOn64;
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

	bool hasAcpiField(size_t length, size_t offset, size_t size) {
		return offset <= length && size <= length - offset;
	}

	template <typename T>
	bool copyAcpiEntry(T *entry, const void *ptr, size_t length, size_t requiredLength) {
		memset(entry, 0, sizeof(T));
		if (length < requiredLength)
			return false;

		memcpy(entry, ptr, frg::min(sizeof(T), length));
		return true;
	}

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

uint32_t affinityFromMpidr(uint64_t mpidr);

bool bootSecondary(
    uint64_t id,
    size_t cpuIndex,
    EnableMethod method,
    frg::optional<uint64_t> releaseAddress = frg::null_opt
) {
	infoLogger() << "thor: Starting CPU with MPIDR " << frg::hex_fmt{id} << frg::endlog;

	// Allocate a stack for the initialization code.
	constexpr size_t stackSize = 0x10000;
	void *stackPtr = kernelAlloc->allocate(stackSize);

	auto *context = getCpuData(cpuIndex);
	context->affinity = affinityFromMpidr(id);

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
	assert(imageSize <= kPageSize - sizeof(StatusBlock));

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
	statusBlock->cpuId = static_cast<int>(cpuIndex);

	bool dontWait = false;

	switch(method) {
		case EnableMethod::spinTable: {
			infoLogger() << "thor: This CPU uses a spin-table" << frg::endlog;

			if (!releaseAddress)
				panicLogger() << "thor: spin-table CPU has no release address" << frg::endlog;
			uint64_t ptr = *releaseAddress;

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

bool bootSecondaryFromDt(DeviceTreeNode *node, size_t cpuIndex) {
	infoLogger() << "thor: Starting CPU \"" << node->path() << "\"" << frg::endlog;

	auto methodProp = node->dtNode().findProperty("enable-method");
	if (!methodProp)
		panicLogger() << node->path() << " has no enable-method" << frg::endlog;

	EnableMethod method = EnableMethod::unknown;
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

	frg::optional<uint64_t> releaseAddress = frg::null_opt;
	if (method == EnableMethod::spinTable) {
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
		releaseAddress = ptr;
	}

	return bootSecondary(node->reg()[0].addr, cpuIndex, method, releaseAddress);
}

bool initPsciFromAcpi() {
	acpi_fadt *fadt;
	if (uacpi_table_fadt(&fadt) != UACPI_STATUS_OK) {
		infoLogger() << "thor: ACPI FADT not found, trying GICC parking protocol"
		             << frg::endlog;
		return false;
	}

	constexpr size_t requiredFadtLength = offsetof(acpi_fadt, arm_boot_arch)
	                                      + sizeof(fadt->arm_boot_arch);
	if (fadt->hdr.length < requiredFadtLength) {
		infoLogger() << "thor: FADT is too small for ARM boot architecture flags" << frg::endlog;
		return false;
	}

	if (!(fadt->arm_boot_arch & ACPI_ARM_PSCI_COMPLIANT)) {
		infoLogger() << "thor: ACPI FADT does not advertise PSCI" << frg::endlog;
		return false;
	}

	psci_.initialize(fadt->arm_boot_arch & ACPI_ARM_PSCI_USE_HVC);
	return true;
}

bool bootApsFromAcpi() {
	auto havePsci = initPsciFromAcpi();

	uacpi_table madtTbl;
	if (uacpi_table_find_by_signature("APIC", &madtTbl) != UACPI_STATUS_OK)
		panicLogger() << "thor: Unable to initialize APs, no MADT found" << frg::endlog;
	frg::scope_exit finish{[&] { uacpi_table_unref(&madtTbl); }};

	auto *madt = reinterpret_cast<acpi_madt *>(madtTbl.ptr);
	auto bspAffinity = getCpuData()->affinity;

	size_t apCpuIndex = 1;
	size_t enabledCpuCount = 0;
	size_t offset = sizeof(acpi_madt);
	while (offset < madt->hdr.length) {
		acpi_entry_hdr generic;
		auto genericPtr = reinterpret_cast<void *>(madtTbl.virt_addr + offset);
		memcpy(&generic, genericPtr, sizeof(generic));
		if (generic.length < sizeof(acpi_entry_hdr))
			panicLogger() << "thor: MADT entry has invalid length " << generic.length
			              << frg::endlog;

		if (generic.type == ACPI_MADT_ENTRY_TYPE_GICC) {
			acpi_madt_gicc entry;
			constexpr size_t requiredGiccLength =
			    offsetof(acpi_madt_gicc, flags) + sizeof(entry.flags);
			if (!copyAcpiEntry(&entry, genericPtr, generic.length, requiredGiccLength)) {
				warningLogger() << "thor: Ignoring truncated MADT GICC entry"
				                << frg::endlog;
				offset += generic.length;
				continue;
			}

			if (entry.flags & ACPI_GICC_ENABLED) {
				++enabledCpuCount;
				if (!hasAcpiField(generic.length, offsetof(acpi_madt_gicc, mpidr),
				        sizeof(entry.mpidr))) {
					panicLogger() << "thor: Enabled MADT GICC entry has no MPIDR"
					              << frg::endlog;
				}

				auto affinity = affinityFromMpidr(entry.mpidr);
				if (affinity != bspAffinity) {
					if (static_cast<uint64_t>(apCpuIndex) >= cpuConfigNote->totalCpus) {
						panicLogger() << "thor: CPU index " << apCpuIndex
						              << " exceeds expected number of CPUs "
						              << cpuConfigNote->totalCpus << frg::endlog;
					}
					if (apCpuIndex < cpuConfigNote->effectiveCpus) {
						if (havePsci) {
							bootSecondary(entry.mpidr, apCpuIndex, EnableMethod::psci);
						} else if (hasAcpiField(generic.length,
						            offsetof(acpi_madt_gicc, parking_protocol_version),
						            sizeof(entry.parking_protocol_version))
						        && hasAcpiField(generic.length,
						            offsetof(acpi_madt_gicc, parked_address),
						            sizeof(entry.parked_address))
						        && entry.parking_protocol_version && entry.parked_address) {
							bootSecondary(
							    entry.mpidr,
							    apCpuIndex,
							    EnableMethod::spinTable,
							    entry.parked_address
							);
						} else {
							panicLogger() << "thor: Enabled MADT GICC entry for MPIDR "
							              << frg::hex_fmt{entry.mpidr}
							              << " has neither PSCI nor parking protocol"
							              << frg::endlog;
						}
					}
					++apCpuIndex;
				}
			}
		}

		offset += generic.length;
	}

	if (enabledCpuCount != cpuConfigNote->totalCpus)
		panicLogger() << "thor: Found " << enabledCpuCount << " CPUs but Eir detected "
		              << cpuConfigNote->totalCpus << frg::endlog;

	return true;
}

static initgraph::Task initAPs{&globalInitEngine, "arm.init-aps",
	initgraph::Requires{
	    acpi::getTablesDiscoveredStage(),
	    getDeviceTreeParsedStage(),
	    getBootProcessorReadyStage(),
	    getTaskingAvailableStage()
	},
	[] {
		if (acpiRsdpNote->rsdp) {
			bootApsFromAcpi();
			return;
		}
		auto root = getDeviceTreeRoot();
		if (!root)
			return;
		root->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<2>({"arm,psci", "arm,psci-1.0"})) {
				psci_.initialize(node);
				return true;
			}
			return false;
		});
		auto bspAffinity = getCpuData()->affinity;
		size_t apCpuIndex = 1;
		auto bootApFromDt = [&](DeviceTreeNode *node) {
			auto affinity = node->reg()[0].addr;
			if (affinity == bspAffinity)
				return;

			if (static_cast<uint64_t>(apCpuIndex) >= cpuConfigNote->totalCpus) {
				panicLogger() << "thor: CPU index " << apCpuIndex
						<< " exceeds expected number of CPUs " << cpuConfigNote->totalCpus
						<< frg::endlog;
			}
			if (apCpuIndex < cpuConfigNote->effectiveCpus)
				bootSecondaryFromDt(node, apCpuIndex);
			++apCpuIndex;
		};
		if (auto it = root->children().find("cpus"); it != root->children().end()) {
			auto cpus = it->get<1>();
			cpus->forEach([&](DeviceTreeNode *node) {
				if (node->name().starts_with("cpu@"))
					bootApFromDt(node);
				return false;
			});
		}

	    if (apCpuIndex != cpuConfigNote->totalCpus)
		    panicLogger() << "thor: Found " << apCpuIndex << " CPUs but Eir detected "
		                  << cpuConfigNote->totalCpus << frg::endlog;
	}
};
}
