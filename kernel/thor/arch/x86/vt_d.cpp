#include <arch/bits.hpp>
#include <arch/variable.hpp>
#include <frg/cmdline.hpp>
#include <frg/span.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/cache.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/physical.hpp>
#include <initgraph.hpp>

#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>

namespace thor {

struct [[gnu::packed]] DmarHeader {
	acpi_sdt_hdr acpi;
	uint8_t host_address_width;
	uint8_t flags;
	uint8_t reserved[10];
};
static_assert(sizeof(DmarHeader) == 48);

enum class DmarRemappingStructureTypes : uint16_t {
	Drhd = 0,
	Rmrr = 1,
};

struct [[gnu::packed]] DmarRemappingStructureType {
	DmarRemappingStructureTypes type;
	uint16_t length;
};
static_assert(sizeof(DmarRemappingStructureType) == 4);

struct [[gnu::packed]] DmarDrhd {
	DmarRemappingStructureType hdr;
	uint8_t flags;
	uint8_t size;
	uint16_t segment;
	uint64_t register_base;
};
static_assert(sizeof(DmarDrhd) == 16);

constexpr uint8_t dmarDrhdFlagsPciIncludeAll = 1;

struct [[gnu::packed]] DmarRmrr {
	DmarRemappingStructureType hdr;
	uint16_t reserved;
	uint16_t segment;
	uint64_t memory_base;
	uint64_t memory_limit;
};
static_assert(sizeof(DmarRmrr) == 24);

struct [[gnu::packed]] DeviceScope {
	uint8_t type;
	uint8_t length;
	uint16_t reserved;
	uint8_t enumeration_id;
	uint8_t start_bus_number;
};
static_assert(sizeof(DeviceScope) == 6);

namespace rootTable {
	constexpr arch::field<uint64_t, bool> present{0, 1};
	constexpr arch::field<uint64_t, uint64_t> contextEntry{12, 52};

	struct alignas(16) Entry {
		arch::bit_variable<uint64_t> entry;
		arch::bit_variable<uint64_t> reserved;
	};
	static_assert(sizeof(Entry) == 16);
}

namespace contextTable {
	enum class TranslationType : uint8_t {
		Passthrough = 0b10,
	};

	// low qword
	constexpr arch::field<uint64_t, bool> present{0, 1};
	constexpr arch::field<uint64_t, TranslationType> translationType{2, 2};

	// high qword
	constexpr arch::field<uint64_t, uint8_t> addressWidth{0, 3};
	constexpr arch::field<uint64_t, uint16_t> domainId{8, 16};

	struct alignas(16) Entry {
		arch::bit_variable<uint64_t> low;
		arch::bit_variable<uint64_t> high;
	};
	static_assert(sizeof(Entry) == 16);
}

namespace sourceIDMasks {
	constexpr arch::field<uint16_t, uint8_t> function{0, 3};
	constexpr arch::field<uint16_t, uint8_t> device{3, 5};
	constexpr arch::field<uint16_t, uint8_t> bus{8, 8};
}

struct SourceID {
	operator uint16_t() {
		return uint16_t{data_.load()};
	}

	uint8_t bus() {
		return data_.load() & sourceIDMasks::bus;
	}

	uint8_t device() {
		return data_.load() & sourceIDMasks::device;
	}

	uint8_t function() {
		return data_.load() & sourceIDMasks::function;
	}

	uint8_t devfn() {
		return (device() << 3) | function();
	}

	SourceID(uint8_t bus, uint8_t slot, uint8_t function) :
	data_{sourceIDMasks::function(function) | sourceIDMasks::device(slot) | sourceIDMasks::bus(bus)} {
	}

	explicit SourceID(uint16_t val) :
	data_{arch::bit_value{val}} { }

private:
	arch::bit_variable<uint16_t> data_;
};
static_assert(sizeof(SourceID) == 2);

namespace regs {
	constexpr arch::bit_register<uint32_t> version{0x00};
	constexpr arch::bit_register<uint64_t> capability{0x08};
	constexpr arch::bit_register<uint64_t> extendedCapability{0x10};
	constexpr arch::bit_register<uint32_t> globalCommand{0x18};
	constexpr arch::bit_register<uint32_t> globalStatus{0x1C};
	constexpr arch::scalar_register<uint64_t> rootEntryTable{0x20};
	constexpr arch::bit_register<uint64_t> contextCommand{0x28};
	constexpr arch::bit_register<uint32_t> faultStatus{0x34};
	constexpr arch::bit_register<uint32_t> faultEventControl{0x38};
	constexpr arch::scalar_register<uint32_t> faultEventData{0x3C};
	constexpr arch::scalar_register<uint32_t> faultEventAddress{0x40};
	constexpr arch::scalar_register<uint32_t> faultEventUpperAddress{0x44};
	constexpr arch::bit_register<uint32_t> protectedMemoryEnable{0x64};

	// IOTLB registers, to be used as offsets into the IOTLB mem_space
	constexpr arch::bit_register<uint64_t> iotlbInvalidateAddress{0x00};
	constexpr arch::bit_register<uint64_t> iotlbInvalidate{0x08};

	// Fault record registers, to be used as offsets into the faultRecord mem_space
	constexpr arch::scalar_register<uint64_t> faultRecordInfo{0x00};
	constexpr arch::bit_register<uint64_t> faultRecordFlags{0x08};
} // namespace regs

namespace version {
	constexpr arch::field<uint32_t, uint8_t> minor{0, 4};
	constexpr arch::field<uint32_t, uint8_t> major{4, 4};
} // namespace version

namespace capability {
	constexpr arch::field<uint64_t, bool> rwbf{4, 1};
	constexpr arch::field<uint64_t, bool> plmr{5, 1};
	constexpr arch::field<uint64_t, bool> phmr{6, 1};
	constexpr arch::field<uint64_t, uint8_t> sagaw{8, 5};
	constexpr arch::field<uint64_t, uint8_t> fro{24, 8};
	constexpr arch::field<uint64_t, uint8_t> nfr{40, 8};
} // namespace capability

namespace extendedCapability {
	constexpr arch::field<uint64_t, bool> coherent{0, 1};
	constexpr arch::field<uint64_t, bool> pt{6, 1};
	constexpr arch::field<uint64_t, uint16_t> ivo{8, 10};
} // namespace extendedCapability

namespace globalStatus {
	constexpr arch::field<uint32_t, bool> interruptRemappingPointerStatus{24, 1};
	constexpr arch::field<uint32_t, bool> writeBufferFlushStatus{27, 1};
	constexpr arch::field<uint32_t, bool> faultLogStatus{29, 1};
	constexpr arch::field<uint32_t, bool> rootTablePointerStatus{30, 1};
	constexpr arch::field<uint32_t, bool> translationEnable{31, 1};

	arch::bit_value<uint32_t> clearedOnCompletion = rootTablePointerStatus(true) |
		writeBufferFlushStatus(true) | faultLogStatus(true) |
		interruptRemappingPointerStatus(true);
} // namespace globalStatus

namespace contextCommand {
	enum class InvalidationGranularity : uint8_t {
		Global = 0b01,
		Domain = 0b10,
		Device = 0b11,
	};

	constexpr arch::field<uint64_t, uint16_t> domainId{0, 16};
	constexpr arch::field<uint64_t, SourceID> sourceId{16, 16};
	constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{61, 2};
	constexpr arch::field<uint64_t, bool> invalidateContextCache{63, 1};
} // namespace contextCommand

namespace faultStatus {
	constexpr arch::field<uint32_t, bool> faultOverflow{0, 1};
	constexpr arch::field<uint32_t, bool> primaryPendingFault{1, 1};
	constexpr arch::field<uint32_t, bool> advancedFaultOverflow{2, 1};
	constexpr arch::field<uint32_t, bool> advancedPendingFault{3, 1};
	constexpr arch::field<uint32_t, bool> invalidationQueueError{4, 1};
	constexpr arch::field<uint32_t, bool> invalidationCompletionError{5, 1};
	constexpr arch::field<uint32_t, bool> invalidationTimeoutError{6, 1};
	constexpr arch::field<uint32_t, uint8_t> faultRecordIndex{8, 8};

	auto stickyBits = faultOverflow(true) | advancedFaultOverflow(true) |
		advancedPendingFault(true) | invalidationQueueError(true) |
		invalidationCompletionError(true) | invalidationTimeoutError(true);
} // namespace faultStatus

namespace faultEventControl {
	constexpr arch::field<uint32_t, bool> interruptMask{31, 1};
	[[maybe_unused]]
	constexpr arch::field<uint32_t, bool> interruptPending{30, 1};
} // namespace faultEventControl

namespace protectedMemoryEnable {
	constexpr arch::field<uint32_t, bool> prs{0, 1};
	constexpr arch::field<uint32_t, bool> epm{31, 1};
} // namespace protectedMemoryEnable

namespace iotlbInvalidate {
	enum class InvalidationGranularity : uint8_t {
		Global = 0b001,
		Domain = 0b010,
		Page = 0b011,
	};

	constexpr arch::field<uint64_t, uint16_t> domainId{32, 16};
	constexpr arch::field<uint64_t, bool> drainWrites{48, 1};
	constexpr arch::field<uint64_t, bool> drainReads{49, 1};
	constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{60, 3};
	constexpr arch::field<uint64_t, bool> invalidateIotlb{63, 1};
} // namespace iotlbInvalidate

namespace faultRecording {
	constexpr arch::field<uint64_t, SourceID> sourceIdentifier{0, 16};
	constexpr arch::field<uint64_t, uint8_t> faultReason{32, 8};
	[[maybe_unused]]
	constexpr arch::field<uint64_t, uint8_t> addressType{60, 2};
	constexpr arch::field<uint64_t, bool> read{62, 1};
	constexpr arch::field<uint64_t, bool> fault{63, 1};
} // namespace faultRecording

namespace {

size_t nextIommuId = 0;

const char *decodeFaultReason(uint8_t reason) {
	switch(reason) {
		case 1: return "Root Entry not present";
		case 2: return "Context Entry not present";
		case 3: return "Invalid Programming of Context Entry";
		default: return "Reserved or Unhandled";
	}
}

} // namespace

struct IntelIommu final : Iommu, IrqSink {
	IntelIommu(uint64_t register_base, uint16_t segment)
	: Iommu(nextIommuId++),
	IrqSink(frg::string(*kernelAlloc, "iommu") +
		frg::to_allocated_string(*kernelAlloc, id())),
	segment_{segment} {
		registerWindow_ = {register_base, 0x1000, CachingMode::mmio};
		regs_ = arch::mem_space{registerWindow_.get()};

		cap_ = regs_.load(regs::capability);
		ecap_ = regs_.load(regs::extendedCapability);
		auto sagaw = cap_ & capability::sagaw;
		assert(sagaw);
		// calculate the value (AW, bits 66:64) we need to set in the translation tables for the
		// highest level of supported page tables, e.g. bit 2 (for 48-bits, 4 levels) this is 2
		sagaw_ = ((sizeof(unsigned int) * 8) - __builtin_clz(sagaw)) + 1;

		auto iotlbOffset = 16 * (ecap_ & extendedCapability::ivo);
		iotlbWindow_ = {register_base + iotlbOffset, 16, CachingMode::mmio};
		iotlb_ = arch::mem_space{iotlbWindow_.get()};

		auto faultRecordOffset = 16 * (cap_ & capability::fro);
		auto faultRecordCount = (cap_ & capability::nfr) + 1UL;
		faultRecordsWindow_ = {register_base + faultRecordOffset, faultRecordCount * 16, CachingMode::mmio};
		faultRecords_ = arch::mem_space{faultRecordsWindow_.get()};

		rootTablePhys_ = physicalAllocator->allocate(0x1000);
		PageAccessor rootTable{rootTablePhys_};
		memset(rootTable.get(), 0, 0x1000);
		flush(rootTable.get(), 0x1000);

		rootTable_ = frg::span{reinterpret_cast<rootTable::Entry *>(rootTable.get()), 0x1000 / sizeof(rootTable::Entry)};
	}

	void init() {
		auto lock = frg::guard(&lock_);

		auto v = regs_.load(regs::version);
		infoLogger() << frg::fmt("thor: IOMMU version {}.{}",
				v & version::major, v & version::minor) << frg::endlog;
		infoLogger() << frg::fmt("thor: DRHD for segment {}", segment_) << frg::endlog;
		infoLogger() << frg::fmt("thor: cap 0x{:016x} ecap 0x{:016x}",
			uint64_t{cap_}, uint64_t{ecap_}) << frg::endlog;

		auto name = frg::string<KernelAlloc>{*kernelAlloc, "iommu"} +
			frg::to_allocated_string(*kernelAlloc, id()) +
			frg::string<KernelAlloc>{*kernelAlloc, "-msi"};

		auto interrupt = allocateApicMsi(name);
		assert(interrupt);
		IrqPin::attachSink(interrupt, this);

		// needs to be done before enabling translation
		writeBufferFlush();

		regs_.store(regs::faultEventControl, faultEventControl::interruptMask(true));

		regs_.store(regs::faultEventAddress, interrupt->getMessageAddress());
		regs_.store(regs::faultEventUpperAddress, interrupt->getMessageAddress() >> 32);
		regs_.store(regs::faultEventData, interrupt->getMessageData());

		regs_.store(regs::faultStatus, faultStatus::stickyBits);
		regs_.store(regs::faultEventControl, faultEventControl::interruptMask(false));

		setRootEntryTable(rootTablePhys_);

		// sanitize firmware state by disabling this (optional) feature
		if(cap_ & capability::plmr || cap_ & capability::phmr) {
			regs_.store(regs::protectedMemoryEnable, regs_.load(regs::protectedMemoryEnable) / protectedMemoryEnable::epm(false));

			while(regs_.load(regs::protectedMemoryEnable) & protectedMemoryEnable::prs);
		}

		setGlobalBit(globalStatus::translationEnable(true));

		initialized_ = true;
	}

	IrqStatus raise() override {
		auto faultRecordCount = (cap_ & capability::nfr) + 1UL;
		auto status = regs_.load(regs::faultStatus);

		if(status & faultStatus::primaryPendingFault) {
			auto faultRecordOffset = status & faultStatus::faultRecordIndex;

			for(size_t i = 0; i < faultRecordCount; i++) {
				auto subspace = faultRecords_.subspace(faultRecordOffset * 16);
				auto flags = subspace.load(regs::faultRecordFlags);

				if(!(flags & faultRecording::fault))
					break;

				auto reason = flags & faultRecording::faultReason;
				auto sourceId = SourceID{flags & faultRecording::sourceIdentifier};

				warningLogger() << frg::fmt("thor: IOMMU fault {}, {} request from {:02x}:{:02x}:{:x} to 0x{:x}: {} (0x{:x})",
					i, (flags & faultRecording::read) ? "Read" : "Write",
					sourceId.bus(), sourceId.device(), sourceId.function(),
					subspace.load(regs::faultRecordInfo),
					decodeFaultReason(reason), reason) << frg::endlog;

				subspace.store(regs::faultRecordFlags, faultRecording::fault(true));

				if(++faultRecordOffset >= faultRecordCount)
					faultRecordOffset = 0;
			}
		}

		if(status & faultStatus::faultOverflow) {
			warningLogger() << "thor: IOMMU fault overflow" << frg::endlog;
		}

		regs_.store(regs::faultStatus, faultStatus::stickyBits);

		return IrqStatus::acked;
	}

	bool supportsPassthrough() const {
		return ecap_ & extendedCapability::pt;
	}

	void enableDevice(pci::PciEntity *dev) override {
		auto lock = frg::guard(&lock_);

		auto rootEntry = &rootTable_[static_cast<uint8_t>(dev->bus)];
		PageAccessor context;
		frg::span<contextTable::Entry> contextTable{};

		if(!(rootEntry->entry.load() & rootTable::present)) {
			auto contextPhys = physicalAllocator->allocate(0x1000);
			context = {contextPhys};

			memset(context.get(), 0, 0x1000);
			flush(context.get(), 0x1000);
			contextTable = {reinterpret_cast<contextTable::Entry *>(context.get()), 256};

			rootEntry->entry.store(rootTable::present(true) | rootTable::contextEntry(contextPhys >> 12));
			flush(rootEntry);
		} else {
			context = {(rootEntry->entry.load() & rootTable::contextEntry) << 12};
			contextTable = {reinterpret_cast<contextTable::Entry *>(context.get()), 256};
		}

		auto sourceId = SourceID{
			static_cast<uint8_t>(dev->bus),
			static_cast<uint8_t>(dev->slot),
			static_cast<uint8_t>(dev->function),
		};

		auto contextEntry = &contextTable[sourceId.devfn()];

		contextEntry->high.store(
			contextTable::addressWidth(sagaw_ - 2) |
			contextTable::domainId(1)
		);

		contextEntry->low.store(
			contextTable::present(true) |
			contextTable::translationType(contextTable::TranslationType::Passthrough)
		);

		flush(contextEntry);

		if(initialized_) {
			invalidateDeviceContext(0, sourceId);
			invalidateDomainIotlb(1);
		}
	}

private:
	void flush(void const *ptr) {
		if(ecap_ & extendedCapability::coherent)
			return;

		cacheFlush(ptr);
	}

	void flush(void const *ptr, size_t len) {
		if(ecap_ & extendedCapability::coherent)
			return;

		cacheFlush(ptr, len);
	}

	void runGlobalCommand(arch::bit_value<uint32_t> c) {
		auto status = regs_.load(regs::globalStatus) & arch::bit_mask(uint32_t(~globalStatus::clearedOnCompletion));
		regs_.store(regs::globalCommand, status | c);

		while(uint32_t(regs_.load(regs::globalStatus) & arch::bit_mask<uint32_t>{uint32_t(c)}));
	}

	void setGlobalBit(arch::bit_value<uint32_t> c) {
		auto status = regs_.load(regs::globalStatus) & arch::bit_mask(uint32_t(~globalStatus::clearedOnCompletion));
		regs_.store(regs::globalCommand, status | c);
	}

	void writeBufferFlush() {
		if((cap_ & capability::rwbf) == 0)
			return;

		runGlobalCommand(globalStatus::writeBufferFlushStatus(true));
	}

	void invalidateGlobalContext() {
		regs_.store(regs::contextCommand, contextCommand::invalidateContextCache(true) |
		contextCommand::invalidationGranularity(contextCommand::InvalidationGranularity::Global));

		while(regs_.load(regs::contextCommand) & contextCommand::invalidateContextCache);
	}

	void invalidateDeviceContext(uint16_t domain, SourceID device) {
		regs_.store(regs::contextCommand, contextCommand::invalidateContextCache(true) |
			contextCommand::invalidationGranularity(contextCommand::InvalidationGranularity::Device) |
			contextCommand::sourceId(device) | contextCommand::domainId(domain));

		while(regs_.load(regs::contextCommand) & contextCommand::invalidateContextCache);
	}

	void invalidateGlobalIotlb() {
		while(iotlb_.load(regs::iotlbInvalidate) & iotlbInvalidate::invalidateIotlb);

		iotlb_.store(regs::iotlbInvalidateAddress, arch::bit_value{0UL});
		iotlb_.store(regs::iotlbInvalidate, iotlbInvalidate::invalidateIotlb(true) |
			iotlbInvalidate::invalidationGranularity(iotlbInvalidate::InvalidationGranularity::Global) |
			iotlbInvalidate::drainReads(true) | iotlbInvalidate::drainWrites(true));

		while(iotlb_.load(regs::iotlbInvalidate) & iotlbInvalidate::invalidateIotlb);
	}

	void invalidateDomainIotlb(uint16_t domain) {
		while(iotlb_.load(regs::iotlbInvalidate) & iotlbInvalidate::invalidateIotlb);

		iotlb_.store(regs::iotlbInvalidateAddress, arch::bit_value{0UL});
		iotlb_.store(regs::iotlbInvalidate, iotlbInvalidate::invalidateIotlb(true) |
			iotlbInvalidate::invalidationGranularity(iotlbInvalidate::InvalidationGranularity::Domain) |
			iotlbInvalidate::drainReads(true) | iotlbInvalidate::drainWrites(true) |
			iotlbInvalidate::domainId(domain));

		while(iotlb_.load(regs::iotlbInvalidate) & iotlbInvalidate::invalidateIotlb);
	}

	void setRootEntryTable(uintptr_t physical) {
		assert((physical & 0xFFF) == 0);
		regs_.store(regs::rootEntryTable, physical);

		setGlobalBit(globalStatus::rootTablePointerStatus(true));

		invalidateGlobalContext();
		invalidateGlobalIotlb();
	}

	bool initialized_ = false;

	IrqSpinlock lock_;

	PhysicalWindow registerWindow_;
	PhysicalWindow iotlbWindow_;
	PhysicalWindow faultRecordsWindow_;

	arch::mem_space regs_;
	arch::mem_space iotlb_;
	arch::mem_space faultRecords_;

	frg::span<rootTable::Entry> rootTable_;
	PhysicalAddr rootTablePhys_;

	uint16_t segment_;

	arch::bit_value<uint64_t> cap_ = arch::bit_value<uint64_t>{0};
	arch::bit_value<uint64_t> ecap_ = arch::bit_value<uint64_t>{0};

	// value for the Context Entry Address Width (AW) field for the highest supported page table level
	uint8_t sagaw_;
};

IntelIommu *handleDrhd(frg::span<uint8_t> remappingStructureTypes) {
	DmarDrhd drhd;
	memcpy(&drhd, remappingStructureTypes.data(), sizeof(drhd));

	auto iommu = frg::construct<IntelIommu>(*kernelAlloc, drhd.register_base, drhd.segment);

	if(!iommu->supportsPassthrough()) {
		infoLogger() << "thor: IOMMU does not support passthrough, ignoring" << frg::endlog;
		return nullptr;
	}

	if(drhd.flags & dmarDrhdFlagsPciIncludeAll) {
		// we need to allow matching multiple root buses, as the spec explicitly
		// allows that to happen
		for(auto b : *pci::allRootBuses) {
			if(b->segId != drhd.segment)
				continue;

			for(auto c : b->childDevices) {
				if(!c->associatedIommu)
					c->associatedIommu = iommu;
			}

			// we treat bridges on the root bus like 'PCI Sub-hierarchy' device scopes,
			// meaning that all bridges and devices behind them are associated with this bridge's IOMMU
			// this allows us to avoid recursively set the associated IOMMU for the children
			for(auto c : b->childBridges) {
				if(!c->associatedIommu)
					c->associatedIommu = iommu;
			}

			break;
		}

		return iommu;
	}

	auto device_scope = remappingStructureTypes.subspan(sizeof(drhd), drhd.hdr.length - sizeof(drhd));

	while(device_scope.size()) {
		if(sizeof(DeviceScope) > device_scope.size())
			break;

		DeviceScope dev;
		memcpy(&dev, device_scope.data(), sizeof(dev));

		if(dev.length < 6 || (dev.length & 1))
			return nullptr;

		pci::PciBus *bus = nullptr;

		for(auto b : *pci::allRootBuses) {
			if(b->segId != drhd.segment)
				continue;
			if(b->busId != dev.start_bus_number)
				continue;

			bus = b;
			break;
		}

		for(size_t pathOffset = 0; pathOffset < (dev.length - 6); pathOffset += 2) {
			switch(dev.type) {
				case 1: {
					if(!bus)
						return nullptr;

					uint8_t slot, func;
					memcpy(&slot, device_scope.subspan(sizeof(DeviceScope) + pathOffset).data(), sizeof(uint8_t));
					memcpy(&func, device_scope.subspan(sizeof(DeviceScope) + pathOffset + 1).data(), sizeof(uint8_t));

					pci::PciDevice *pciDev = nullptr;
					for(auto d : bus->childDevices) {
						if(d->slot == slot && d->function == func) {
							pciDev = d;
							break;
						}
					}

					if(!pciDev)
						return nullptr;

					pciDev->associatedIommu = iommu;
					break;
				}
				case 2: {
					if(!bus)
						return nullptr;

					uint8_t slot, func;
					memcpy(&slot, device_scope.subspan(sizeof(DeviceScope) + pathOffset).data(), sizeof(uint8_t));
					memcpy(&func, device_scope.subspan(sizeof(DeviceScope) + pathOffset + 1).data(), sizeof(uint8_t));

					pci::PciBus *newbus = nullptr;
					for(auto b : bus->childBridges) {
						if(b->slot != slot)
							continue;
						if(b->function != func)
							continue;

						newbus = b->associatedBus;
						b->associatedIommu = iommu;
						break;
					}

					if(!newbus || newbus == bus)
						return nullptr;

					bus = newbus;

					break;
				}
				default:
					infoLogger() << frg::fmt("thor: unhandled DMAR device scope type {}", dev.type) << frg::endlog;
					break;
			}
		}

		if(dev.length >= device_scope.size())
			break;

		device_scope = device_scope.subspan(dev.length);
	}

	return iommu;
}

bool handleRmrr(frg::span<uint8_t> remappingStructureTypes) {
	DmarRmrr rmrr;
	memcpy(&rmrr, remappingStructureTypes.data(), sizeof(rmrr));

	auto device_scope = remappingStructureTypes.subspan(sizeof(rmrr), rmrr.hdr.length - sizeof(rmrr));

	while(device_scope.size()) {
		if(sizeof(DeviceScope) > device_scope.size())
			break;

		DeviceScope dev;
		memcpy(&dev, device_scope.data(), sizeof(dev));

		assert(dev.length >= 6);
		assert((dev.length & 1) == 0);

		pci::PciBus *bus = nullptr;

		for(auto b : *pci::allRootBuses) {
			if(b->segId != rmrr.segment)
				continue;
			if(b->busId != dev.start_bus_number)
				continue;

			bus = b;
			break;
		}

		for(size_t pathOffset = 0; pathOffset < (dev.length - 6); pathOffset += 2) {
			switch(dev.type) {
				case 1: {
					if(!bus)
						return false;

					uint8_t slot, func;
					memcpy(&slot, device_scope.subspan(sizeof(DeviceScope) + pathOffset).data(), sizeof(uint8_t));
					memcpy(&func, device_scope.subspan(sizeof(DeviceScope) + pathOffset + 1).data(), sizeof(uint8_t));

					pci::PciDevice *pciDev = nullptr;
					for(auto d : bus->childDevices) {
						if(d->slot == slot && d->function == func) {
							pciDev = d;
							break;
						}
					}

					if(!pciDev)
						return false;

					infoLogger() << frg::fmt("thor: PCI device {:04x}:{:02x}:{:02x}.{} has RMRR",
						rmrr.segment, dev.start_bus_number, slot, func) << frg::endlog;

					if(pciDev->associatedIommu) {
						pciDev->associatedIommu->enableDevice(pciDev);
					} else {
						auto bridge = pciDev->parentBus->associatedBridge;

						while(bridge && bridge->parentBus && !bridge->associatedIommu)
							bridge = bridge->parentBus->associatedBridge;

						if(bridge && bridge->associatedIommu)
							bridge->associatedIommu->enableDevice(bridge);
						else
							infoLogger() << frg::fmt("thor: no bridge with associated IOMMU for {:04x}:{:02x}:{:02x}.{}",
								rmrr.segment, dev.start_bus_number, slot, func) << frg::endlog;
					}

					break;
				}
				case 2: {
					if(!bus)
						return false;

					uint8_t slot, func;
					memcpy(&slot, device_scope.subspan(sizeof(DeviceScope) + pathOffset).data(), sizeof(uint8_t));
					memcpy(&func, device_scope.subspan(sizeof(DeviceScope) + pathOffset + 1).data(), sizeof(uint8_t));

					pci::PciBus *newbus = nullptr;
					for(auto b : bus->childBridges) {
						if(b->slot != slot)
							continue;
						if(b->function != func)
							continue;

						newbus = b->associatedBus;
						break;
					}

					if(!newbus || newbus == bus)
						return false;

					bus = newbus;

					infoLogger() << frg::fmt("thor: PCI bridge at {:04x}:{:02x}:{:02x}.{} to bus {} has RMRR",
						rmrr.segment, dev.start_bus_number, slot, func, bus->busId) << frg::endlog;

					auto bridge = bus->associatedBridge;

					while(bridge && bridge->parentBus && !bridge->associatedIommu)
						bridge = bridge->parentBus->associatedBridge;

					if(bridge && bridge->associatedIommu)
						bridge->associatedIommu->enableDevice(bridge);
					else
						infoLogger() << frg::fmt("thor: no bridge with associated IOMMU for {:04x}:{:02x}:{:02x}.{}",
							rmrr.segment, dev.start_bus_number, slot, func) << frg::endlog;

					break;
				}
				default:
					infoLogger() << frg::fmt("thor: device scope type {}", dev.type) << frg::endlog;
					break;
			}
		}

		if(dev.length > device_scope.size())
			break;

		device_scope = device_scope.subspan(dev.length);
	}

	return true;
}

static initgraph::Task discoverConfigIoSpaces{&globalInitEngine, "x86.discover-intel-iommu",
	initgraph::Requires{acpi::getTablesDiscoveredStage(), pci::getDevicesEnumeratedStage()},
	[] {
		uacpi_table dmarTbl;
		frg::string_view iommuState = "on";

		frg::array args = {
			frg::option{"iommu", frg::as_string_view(iommuState)},
		};
		frg::parse_arguments(getKernelCmdline(), args);

		if(iommuState != "on") {
			infoLogger() << "thor: IOMMU disabled by command line" << frg::endlog;
			return;
		}

		auto ret = uacpi_table_find_by_signature("DMAR", &dmarTbl);
		if(ret == UACPI_STATUS_NOT_FOUND) {
			infoLogger() << "thor: No DMAR table!" << frg::endlog;
			return;
		}

		auto hdr = reinterpret_cast<DmarHeader *>(dmarTbl.ptr);

		infoLogger() << frg::fmt("thor: DMAR host address width {}", hdr->host_address_width + 1) << frg::endlog;

		frg::span<uint8_t> remappingStructureTypes{reinterpret_cast<uint8_t *>(dmarTbl.virt_addr + sizeof(DmarHeader)), dmarTbl.hdr->length - sizeof(DmarHeader)};
		frg::vector<IntelIommu *, KernelAlloc> iommus{*kernelAlloc};

		while(remappingStructureTypes.size()) {
			if(sizeof(DmarRemappingStructureType) > remappingStructureTypes.size())
				break;

			DmarRemappingStructureType hdr;
			memcpy(&hdr, remappingStructureTypes.data(), sizeof(hdr));

			switch(hdr.type) {
				case DmarRemappingStructureTypes::Drhd: {
					auto iommu = handleDrhd(remappingStructureTypes);
					if(iommu)
						iommus.push_back(iommu);
					else
						warningLogger() << frg::fmt("thor: skipping IOMMU due to invalid DRHD") << frg::endlog;
					break;
				}
				case DmarRemappingStructureTypes::Rmrr: {
					if(!handleRmrr(remappingStructureTypes)) {
						warningLogger() << frg::fmt("thor: skipping IOMMU setup due to invalid RMRR") << frg::endlog;
						return;
					}
					break;
				}
				default:
					infoLogger() << frg::fmt("thor: unhandled remapping structure type {}",
						static_cast<uint16_t>(hdr.type)) << frg::endlog;
					break;
			}

			if(hdr.length > remappingStructureTypes.size())
				break;

			remappingStructureTypes = remappingStructureTypes.subspan(hdr.length);
		}

		for(auto iommu : iommus) {
			iommu->init();
		}
	}
};

}
