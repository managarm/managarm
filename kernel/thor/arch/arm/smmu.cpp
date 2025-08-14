#include <thor-internal/arch/smmu.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/iommu.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/pci/pci_iommu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/paging.hpp>

#include <frg/optional.hpp>

namespace thor {

static inline frg::array<frg::string_view, 3> dtSmmuV2Compatible{
	"arm,smmu-v2",
	"arm,mmu-500",
	"qcom,qsmmu-v500"
};

static inline frg::array<frg::string_view, 1> skipInitProps{
	"qcom,skip-init"
};

namespace regs0 {
	static constexpr arch::bit_register<uint32_t> cr0{0};
	static constexpr arch::bit_register<uint32_t> idr0{0x20};
	static constexpr arch::bit_register<uint32_t> idr1{0x24};
	static constexpr arch::scalar_register<uint64_t> gfar{0x40};
	static constexpr arch::bit_register<uint32_t> gfsr{0x48};
	static constexpr arch::scalar_register<uint32_t> tlbiallnsnh{0x68};
	static constexpr arch::scalar_register<uint32_t> tlbiallh{0x6c};
	static constexpr arch::scalar_register<uint32_t> tlbgsync{0x70};
	static constexpr arch::bit_register<uint32_t> tlbgstatus{0x74};
	static constexpr arch::bit_register<uint32_t> s2cr{0xc00};
	static constexpr arch::bit_register<uint32_t> smr{0x800};
} // namespace regs0

namespace regs1 {
	static constexpr arch::bit_register<uint32_t> cbar{0};
	static constexpr arch::bit_register<uint32_t> cba2r{0x800};
} // namespace regs1

namespace cbRegs {
	static constexpr arch::bit_register<uint32_t> sctlr{0};
	static constexpr arch::scalar_register<uint64_t> ttbr0{0x20};
	static constexpr arch::scalar_register<uint64_t> ttbr1{0x28};
	static constexpr arch::scalar_register<uint32_t> tcr{0x30};
	static constexpr arch::scalar_register<uint32_t> mair0{0x38};
	static constexpr arch::scalar_register<uint32_t> mair1{0x3c};
	static constexpr arch::bit_register<uint32_t> fsr{0x58};
	static constexpr arch::scalar_register<uint64_t> far{0x60};
} // namespace cbRegs

namespace cr0 {
	static constexpr arch::field<uint32_t, bool> clientPortDisable{0, 1};
	static constexpr arch::field<uint32_t, bool> globalFaultReportEnable{1, 1};
	static constexpr arch::field<uint32_t, bool> globalFaultInterruptEnable{2, 1};
	static constexpr arch::field<uint32_t, bool> extendedIdEnable{3, 1};
	static constexpr arch::field<uint32_t, bool> globalConfigFaultReportEnable{4, 1};
	static constexpr arch::field<uint32_t, bool> globalConfigFaultInterruptEnable{5, 1};
	static constexpr arch::field<uint32_t, bool> faultOnUnidentifiedStream{10, 1};
	static constexpr arch::field<uint32_t, bool> vmidPrivateNamespaceEnable{11, 1};
	static constexpr arch::field<uint32_t, bool> privateTlbMaintenance{12, 1};
	static constexpr arch::field<uint32_t, bool> forceBroadcastTlbMaintenance{13, 1};
	static constexpr arch::field<uint32_t, uint8_t> barrierShareabilityUpgrade{14, 2};
	static constexpr arch::field<uint32_t, uint8_t> sharedConfiguration{22, 2};
	static constexpr arch::field<uint32_t, bool> enable16BitVmid{31, 1};
} // namespace cr0

namespace idr0 {
	static constexpr arch::field<uint32_t, uint8_t> numStreamMappingRegGroups{0, 8};
	static constexpr arch::field<uint32_t, bool> extendedIdSupport{8, 1};
	static constexpr arch::field<uint32_t, uint8_t> streamIdBits{9, 4};
	static constexpr arch::field<uint32_t, uint8_t> numContextInterrupts{16, 8};
} // namespace idr0

namespace idr1 {
	static constexpr arch::field<uint32_t, uint8_t> numContextBanks{0, 8};
	static constexpr arch::field<uint32_t, uint8_t> numStage2ContextBanks{16, 8};
	static constexpr arch::field<uint32_t, uint8_t> numPageIndexBits{28, 3};
	static constexpr arch::field<uint32_t, bool> pageSize{31, 1};
} // namespace idr1

namespace gfsr {
	static constexpr arch::field<uint32_t, bool> icf{0, 1};
	static constexpr arch::field<uint32_t, bool> usf{1, 1};
	static constexpr arch::field<uint32_t, bool> smcf{2, 1};
	static constexpr arch::field<uint32_t, bool> ucbf{3, 1};
	static constexpr arch::field<uint32_t, bool> ucif{4, 1};
	static constexpr arch::field<uint32_t, bool> caf{5, 1};
	static constexpr arch::field<uint32_t, bool> ef{6, 1};
	static constexpr arch::field<uint32_t, bool> pf{7, 1};
	static constexpr arch::field<uint32_t, bool> uut{8, 1};
	static constexpr arch::field<uint32_t, bool> multi{31, 1};

	static auto clearValue =
		icf(true) | usf(true) | smcf(true) | ucbf(true) | ucif(true)
		| caf(true) | ef(true) | pf(true) | uut(true) | multi(true);
} // namespace gfsr

enum class TranslationType : uint8_t {
	translate = 0,
	bypass = 1,
	fault = 2
};

namespace tlbgstatus {
	static constexpr arch::field<uint32_t, bool> synchronizeTlbActive{0, 1};
} // namespace tlbgstatus

namespace s2cr {
	static constexpr arch::field<uint32_t, uint8_t> contextBankIndex{0, 8};
	static constexpr arch::field<uint32_t, bool> extendedIdValid{10, 1};
	static constexpr arch::field<uint32_t, TranslationType> type{16, 2};
} // namespace s2cr

namespace smr {
	static constexpr arch::field<uint32_t, uint16_t> id{0, 16};
	static constexpr arch::field<uint32_t, uint16_t> mask{16, 16};
	static constexpr arch::field<uint32_t, bool> valid{31, 1};
} // namespace smr

enum class CbarType : uint8_t {
	stage2Only = 0,
	stage1Stage2Bypass = 1,
	stage1Stage2Fault = 2,
	stage1Stage2 = 3
};

namespace cbar {
	static constexpr arch::field<uint32_t, uint8_t> vmid{0, 8};
	static constexpr arch::field<uint32_t, CbarType> type{16, 2};
	static constexpr arch::field<uint32_t, uint8_t> interruptIndex{24, 8};
} // namespace cbar

namespace cba2r {
	static constexpr arch::field<uint32_t, bool> va64{0, 1};
} // namespace cba2r

enum class ContextFaultConfig : uint8_t {
	terminate = 0,
	stall = 1
};

namespace cb_sctlr {
	static constexpr arch::field<uint32_t, bool> mmuEnable{0, 1};
	static constexpr arch::field<uint32_t, bool> contextFaultReportEnable{5, 1};
	static constexpr arch::field<uint32_t, bool> contextFaultInterruptEnable{6, 1};
	static constexpr arch::field<uint32_t, ContextFaultConfig> contextFaultConfig{7, 1};
} // namespace cb_sctlr

namespace cb_fsr {
	static constexpr arch::field<uint32_t, bool> tf{1, 1};
	static constexpr arch::field<uint32_t, bool> aff{2, 1};
	static constexpr arch::field<uint32_t, bool> pf{3, 1};
	static constexpr arch::field<uint32_t, bool> ef{4, 1};
	static constexpr arch::field<uint32_t, bool> tlbmcf{5, 1};
	static constexpr arch::field<uint32_t, bool> tlblkf{6, 1};
	static constexpr arch::field<uint32_t, bool> asf{7, 1};
	static constexpr arch::field<uint32_t, bool> uut{8, 1};
	static constexpr arch::field<uint32_t, bool> ss{30, 1};
	static constexpr arch::field<uint32_t, bool> multi{31, 1};

	static auto clearValue =
		tf(true) | aff(true) | pf(true) | ef(true) | tlbmcf(true)
		| tlblkf(true) | asf(true) | uut(true) | multi(true);
} // namespace cb_fsr

namespace {

size_t nextIommuId = 0;

}

struct SmmuV2 : Iommu {
	struct GlobalIrq : IrqSink {
		GlobalIrq(SmmuV2 *parent, uint32_t index, IrqPin *pin)
		: IrqSink{frg::string(*kernelAlloc, "smmu-global-irq") +
			frg::to_allocated_string(*kernelAlloc, index)}, parent{parent},
			index{index} {
			IrqPin::attachSink(pin, this);
		}

		IrqStatus raise() override {
			auto status = parent->globalSpace0.load_relaxed(regs0::gfsr);
			if (static_cast<uint32_t>(status) == 0) {
				return IrqStatus::nacked;
			}

			auto far = parent->globalSpace0.load_relaxed(regs0::gfar);

			infoLogger() << "thor: SMMU global irq " << index
					<< ", status 0x" << frg::hex_fmt{static_cast<uint32_t>(status)}
					<< ", fault address 0x" << frg::hex_fmt{far}
					<< frg::endlog;

			parent->globalSpace0.store_relaxed(regs0::gfsr, status);

			return IrqStatus::acked;
		}

		void dumpHardwareState() override {
			infoLogger() << "thor: SMMU global irq " << index << frg::endlog;
		}

		SmmuV2 *parent;
		uint32_t index;
	};

	struct ContextBankIrq : IrqSink {
		ContextBankIrq(SmmuV2 *parent, uint32_t index, IrqPin *pin)
		: IrqSink{frg::string(*kernelAlloc, "smmu-ctx-bank-irq") +
			frg::to_allocated_string(*kernelAlloc, index)}, parent{parent},
			index{index}, used{false} {
			IrqPin::attachSink(pin, this);
		}

		IrqStatus raise() override {
			auto cbSpace = parent->contextBankBase.subspace(parent->pageSize * index);

			auto status = cbSpace.load_relaxed(cbRegs::fsr);
			if (static_cast<uint32_t>(status) == 0) {
				return IrqStatus::nacked;
			}

			auto far = cbSpace.load_relaxed(cbRegs::far);

			infoLogger() << "thor: SMMU context bank irq " << index
					<< ", status 0x" << frg::hex_fmt{static_cast<uint32_t>(status)}
					<< ", fault address 0x" << frg::hex_fmt{far}
					<< frg::endlog;

			cbSpace.store_relaxed(cbRegs::fsr, status);

			return IrqStatus::acked;
		}

		void dumpHardwareState() override {
			infoLogger() << "thor: SMMU context bank irq " << index << frg::endlog;
		}

		SmmuV2 *parent;
		uint32_t index;
		bool used;
	};

	SmmuV2(DeviceTreeNode *node, frg::vector<IrqPin *, KernelAlloc> irqPins)
	: Iommu{nextIommuId++}, node {node},
			globalIrqs{*kernelAlloc}, contextBankIrqs{*kernelAlloc} {
		assert(node->reg().size() >= 1);
		auto &reg = node->reg();

		auto ptr = KernelVirtualMemory::global().allocate(reg[0].size);
		for (size_t i = 0; i < reg[0].size; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(VirtualAddr(ptr) + i, reg[0].addr + i,
					page_access::write, CachingMode::mmio);
		}
		globalSpace0 = arch::mem_space{ptr};

		uint32_t numGlobalIrqs = 0;
		if (auto globalIrqsProp = node->dtNode().findProperty("#global-interrupts")) {
			numGlobalIrqs = globalIrqsProp->asU32();
		} else {
			panicLogger() << "thor: SMMU node is missing #global-interrupts" << frg::endlog;
		}

		assert(numGlobalIrqs >= 1);

		auto globalIrq = frg::construct<GlobalIrq>(*kernelAlloc, this, 0, irqPins[0]);
		globalIrqs.push(globalIrq);

		auto idr0 = globalSpace0.load_relaxed(regs0::idr0);
		auto idr1 = globalSpace0.load_relaxed(regs0::idr1);

		auto numContextInterrupts = idr0 & idr0::numContextInterrupts;
		if (numContextInterrupts == 1) {
			dedicateContextBankInterrupts = true;

			contextBankIrqs.resize(irqPins.size() - numGlobalIrqs);
			for (size_t i = numGlobalIrqs; i < irqPins.size(); ++i) {
				auto contextBankIrq = frg::construct<ContextBankIrq>(
						*kernelAlloc, this, i - numGlobalIrqs, irqPins[i]);
				contextBankIrqs[i - numGlobalIrqs] = contextBankIrq;
			}
		} else {
			dedicateContextBankInterrupts = false;

			contextBankIrqs.resize(numContextInterrupts);
			assert(numContextInterrupts <= irqPins.size() - numGlobalIrqs);
			for (size_t i = numGlobalIrqs; i < irqPins.size(); ++i) {
				auto contextBankIrq = frg::construct<ContextBankIrq>(
						*kernelAlloc, this, i - numGlobalIrqs, irqPins[i]);
				contextBankIrqs[i - numGlobalIrqs] = contextBankIrq;
			}
		}

		numStreamMappingRegGroups = idr0 & idr0::numStreamMappingRegGroups;

		if (idr0 & idr0::extendedIdSupport) {
			assert((idr0 & idr0::streamIdBits) == 15);
			maxStreamId = 0xffff;
		} else {
			auto streamIdBits = idr0 & idr0::streamIdBits;
			maxStreamId = (1U << streamIdBits) - 1;
		}

		numContextBanks = idr1 & idr1::numContextBanks;
		numStage2ContextBanks = idr1 & idr1::numStage2ContextBanks;
		uint8_t pageShift = (idr1 & idr1::pageSize) ? 16 : 12;
		pageSize = 1U << pageShift;

		infoLogger() << "thor: Found SMMU at node \"" << node->path() << "\", "
				<< numStreamMappingRegGroups << " streams, "
				<< numContextBanks << " context banks"
				<< frg::endlog;

		globalSpace1 = globalSpace0.subspace(pageSize);
		uintptr_t globalAddressSpacePages = 1U << ((idr1 & idr1::numPageIndexBits) + 1);
		contextBankBase = globalSpace0.subspace(globalAddressSpacePages << pageShift);

		reset();

		auto cr0 = globalSpace0.load_relaxed(regs0::cr0);

		cr0 &= ~cr0::clientPortDisable;
		cr0 |= cr0::globalFaultReportEnable(true);
		cr0 |= cr0::globalFaultInterruptEnable(true);
		cr0 |= cr0::globalConfigFaultReportEnable(true);
		cr0 |= cr0::globalConfigFaultInterruptEnable(true);
		cr0 &= ~cr0::faultOnUnidentifiedStream;
		cr0 |= cr0::vmidPrivateNamespaceEnable(true);
		cr0 |= cr0::privateTlbMaintenance(true);
		cr0 &= ~cr0::forceBroadcastTlbMaintenance;
		cr0 &= ~cr0::barrierShareabilityUpgrade;
		cr0 &= ~cr0::enable16BitVmid;

		if (maxStreamId == 0xffff) {
			cr0 |= cr0::extendedIdEnable(true);
		}

		globalSpace0.store_relaxed(regs0::cr0, cr0);

		invalidateWholeTlb();

		auto freeContextBank = findFreeContextBank();
		assert(freeContextBank);

		identityContextBankIndex = *freeContextBank;

		uint8_t identityInterruptIndex = 0;
		if (!dedicateContextBankInterrupts) {
			auto irqResult = findFreeContextInterrupt();
			assert(irqResult);
			identityInterruptIndex = irqResult.value()->index;
		}

		ContextBankInfo info{
			.type = CbarType::stage1Stage2Bypass,
			.vmId = 0,
			.interruptIndex = identityInterruptIndex,
			.ttbr{},
			.mair{},
			.mmuEnable = false,
			.faultReportEnable = true,
			.faultInterruptEnable = true,
			.faultConfig = ContextFaultConfig::terminate
		};

		configureContextBank(identityContextBankIndex, info);
	}

	void reset() {
		// clear global faults
		globalSpace0.store_relaxed(regs0::gfsr, globalSpace0.load_relaxed(regs0::gfsr));

		if (hasProperty(skipInitProps)) {
			infoLogger() << "thor: Skipping SMMU reset due to a skip property" << frg::endlog;
			return;
		}

		infoLogger() << "thor: Resetting SMMU" << frg::endlog;

		for (uint32_t i = numStage2ContextBanks; i < numStreamMappingRegGroups; ++i) {
			configureStreamGroup(i, 0, 0, 0, TranslationType::bypass, false);
		}

		ContextBankInfo resetConfig{
			.type = CbarType::stage2Only,
			.vmId = 0,
			.interruptIndex = 0,
			.ttbr{},
			.mair{},
			.mmuEnable = false,
			.faultReportEnable = false,
			.faultInterruptEnable = false,
			.faultConfig = ContextFaultConfig::terminate
		};

		for (uint32_t i = numStage2ContextBanks; i < numContextBanks; ++i) {
			configureContextBank(i, resetConfig);

			auto cbSpace = contextBankBase.subspace(pageSize * i);
			cbSpace.store_relaxed(cbRegs::fsr, cb_fsr::clearValue);
		}
	}

	void invalidateWholeTlb() {
		// the values have to be non-zero to work around hypervisor bugs.
		globalSpace0.store_relaxed(regs0::tlbiallnsnh, -1);
		globalSpace0.store_relaxed(regs0::tlbiallh, -1);

		// synchronize
		globalSpace0.store_relaxed(regs0::tlbgsync, -1);

		for (int i = 0;; ++i) {
			if (!(globalSpace0.load_relaxed(regs0::tlbgstatus) & tlbgstatus::synchronizeTlbActive)) {
				break;
			} else if (i == 1000 * 500) {
				warningLogger() << "thor: SMMU TLB synchronization timed out after 1s!" << frg::endlog;
				break;
			}

			KernelFiber::asyncBlockCurrent(
				generalTimerEngine()->sleepFor(1000)
			);
		}
	}

	void configureStreamGroup(uint8_t index, uint16_t streamId, uint16_t streamMask,
			uint8_t contextBankIndex, TranslationType type, bool valid) {
		arch::bit_value<uint32_t> smr{0};
		arch::bit_value<uint32_t> s2cr{0};

		if (maxStreamId != 0xffff) {
			smr = smr::id(streamId) | smr::mask(streamMask) | smr::valid(valid);
			s2cr = s2cr::contextBankIndex(contextBankIndex) | s2cr::type(type);
		} else {
			// extended id format
			smr = smr::id(streamId) | smr::mask(streamMask);
			s2cr = s2cr::contextBankIndex(contextBankIndex) | s2cr::type(type) | s2cr::extendedIdValid(valid);
		}

		globalSpace0.store_relaxed(regs0::smr, index * 4, smr);
		globalSpace0.store_relaxed(regs0::s2cr, index * 4, s2cr);

		asm volatile("dsb st");
	}

	struct ContextBankInfo {
		CbarType type;
		uint8_t vmId;
		uint8_t interruptIndex;
		uint64_t ttbr[2];
		uint64_t mair[2];
		bool mmuEnable;
		bool faultReportEnable;
		bool faultInterruptEnable;
		ContextFaultConfig faultConfig;
	};

	void configureContextBank(uint8_t index, const ContextBankInfo &info) {
		auto cbar = globalSpace1.load_relaxed(regs1::cbar, index * 4);
		cbar &= ~cbar::type;
		cbar &= ~cbar::vmid;
		cbar &= ~cbar::interruptIndex;
		cbar |= cbar::type(info.type);
		cbar |= cbar::vmid(info.vmId);
		cbar |= cbar::interruptIndex(info.interruptIndex);
		globalSpace1.store_relaxed(regs1::cbar, index * 4, cbar);

		auto cba2r = globalSpace1.load_relaxed(regs1::cba2r, index * 4);
		cba2r |= cba2r::va64(true);
		globalSpace1.store_relaxed(regs1::cba2r, index * 4, cba2r);

		auto cbSpace = contextBankBase.subspace(pageSize * index);
		cbSpace.store_relaxed(cbRegs::ttbr0, info.ttbr[0]);
		cbSpace.store_relaxed(cbRegs::ttbr1, info.ttbr[1]);
		cbSpace.store_relaxed(cbRegs::mair0, info.mair[0]);
		cbSpace.store_relaxed(cbRegs::mair1, info.mair[1]);

		auto sctlr = cb_sctlr::mmuEnable(info.mmuEnable)
				| cb_sctlr::contextFaultReportEnable(info.faultReportEnable)
				| cb_sctlr::contextFaultInterruptEnable(info.faultInterruptEnable)
				| cb_sctlr::contextFaultConfig(info.faultConfig);
		cbSpace.store_relaxed(cbRegs::sctlr, sctlr);
		cbSpace.store_relaxed(cbRegs::tcr, 0);
		asm volatile("dsb st");
	}

	bool hasProperty(frg::span<frg::string_view> props) {
		for (auto prop : props) {
			if (node->dtNode().findProperty(prop)) {
				return true;
			}
		}

		return false;
	}

	frg::optional<uint32_t> findFreeContextBank() {
		for (uint32_t i = 0; i < numContextBanks; ++i) {
			auto cbar = globalSpace1.load_relaxed(regs1::cbar, i * 4);

			auto type = cbar & cbar::type;
			auto vmid = cbar & cbar::vmid;

			if (type == CbarType::stage2Only || (type == CbarType::stage1Stage2Bypass && vmid == 0xff)) {
				return i;
			}
		}

		return frg::null_opt;
	}

	frg::optional<uint32_t> findFreeStreamMappingGroup() {
		for (uint32_t i = numStage2ContextBanks; i < numStreamMappingRegGroups; ++i) {
			auto smr = globalSpace0.load_relaxed(regs0::smr, i * 4);
			if (!(smr & smr::valid)) {
				return i;
			}
		}

		return frg::null_opt;
	}

	frg::optional<uint32_t> findConfiguredStreamMapping(uint16_t streamId) {
		for (uint32_t i = 0; i < numStreamMappingRegGroups; ++i) {
			auto value = globalSpace0.load_relaxed(regs0::smr, i * 4);
			if ((value & smr::valid) && (value & smr::id) == streamId) {
				return i;
			}
		}

		return frg::null_opt;
	}

	frg::optional<ContextBankIrq *> findFreeContextInterrupt() {
		for (auto irq : contextBankIrqs) {
			if (!irq->used) {
				return irq;
			}
		}

		return frg::null_opt;
	}

	void enableDevice(pci::PciEntity *dev) override {
		pci::RequestID requestId{
			static_cast<uint8_t>(dev->bus),
			static_cast<uint8_t>(dev->slot),
			static_cast<uint8_t>(dev->function),
		};

		// SMMU v2 only supports at maximum 16-bit stream identifiers, leaving no space for a segment.
		assert(dev->seg == 0);

		uint16_t streamId = static_cast<uint16_t>(requestId);
		assert(streamId <= maxStreamId);

		auto streamIndexRes = findConfiguredStreamMapping(streamId);
		if (streamIndexRes) {
			infoLogger() << "thor: SMMU already configured for stream 0x"
					<< frg::hex_fmt{streamId} << frg::endlog;
			return;
		}

		infoLogger() << "thor: Configuring SMMU stream 0x" << frg::hex_fmt{streamId} << frg::endlog;

		streamIndexRes = findFreeStreamMappingGroup();
		assert(streamIndexRes);

		configureStreamGroup(
				*streamIndexRes, streamId, 0,
				identityContextBankIndex,
				TranslationType::translate, true);
	}

	void enableDevice(DeviceTreeNode *dev, const DeviceTreeProperty &iommuProp) override {
		infoLogger() << "thor: Configuring SMMU for node \"" << dev->path() << "\"" << frg::endlog;

		auto iommuCells = node->iommuCells();
		uint32_t streamId = 0;
		uint32_t streamMask = 0;
		if (iommuCells == 1) {
			streamId = iommuProp.asU32(0);
		} else if (iommuCells == 2) {
			streamId = iommuProp.asU32(0);
			streamMask = iommuProp.asU32(4);
		} else {
			panicLogger() << "thor: Invalid SMMU iommu cells value "
					<< iommuCells << frg::endlog;
		}

		assert(streamId <= maxStreamId);
		assert(streamMask <= (maxStreamId == 0xffff ? 0xffff : 0x7fff));

		auto streamIndexRes = findConfiguredStreamMapping(streamId);
		if (streamIndexRes) {
			infoLogger() << "thor: SMMU already configured for stream 0x"
					<< frg::hex_fmt{streamId} << frg::endlog;
			return;
		}

		infoLogger() << "thor: Configuring SMMU stream 0x" << frg::hex_fmt{streamId} << frg::endlog;

		streamIndexRes = findFreeStreamMappingGroup();
		assert(streamIndexRes);

		configureStreamGroup(
				*streamIndexRes, streamId, streamMask,
				identityContextBankIndex,
				TranslationType::translate, true);
	}

	DeviceTreeNode *node;
	arch::mem_space globalSpace0;
	arch::mem_space globalSpace1;
	uint32_t pageSize;
	uint32_t numStreamMappingRegGroups;
	uint32_t maxStreamId;
	uint32_t numContextBanks;
	uint32_t numStage2ContextBanks;
	arch::mem_space contextBankBase;

	uint32_t identityContextBankIndex;

	frg::vector<GlobalIrq *, KernelAlloc> globalIrqs;
	frg::vector<ContextBankIrq *, KernelAlloc> contextBankIrqs;
	bool dedicateContextBankInterrupts;
};

static initgraph::Task initSmmu{&globalInitEngine, "arm.init-smmu",
	initgraph::Requires{getTaskingAvailableStage()},
	initgraph::Entails{getSmmuReadyStage()},
	[] {
		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible(dtSmmuV2Compatible)) {
				frg::vector<IrqPin *, KernelAlloc> irqPins{*kernelAlloc};

				auto walkInterruptResult = dt::walkInterrupts(
					[&] (DeviceTreeNode *parentNode, dtb::Cells irqCells) {
						auto pin = parentNode->getAssociatedIrqController()->resolveDtIrq(irqCells);
						irqPins.push(pin);
					}, node);

				assert(walkInterruptResult && walkInterruptResult.value() && "Failed to parse SMMU interrupts");

				auto smmu = frg::construct<SmmuV2>(*kernelAlloc, node, std::move(irqPins));
				node->associateIommu(smmu);
			}

			return false;
		});
	}
};

initgraph::Stage *getSmmuReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.smmu-ready"};
	return &s;
}

} // namespace thor
