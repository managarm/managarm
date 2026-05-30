#include <arch/bits.hpp>
#include <arch/variable.hpp>
#include <async/recurring-event.hpp>
#include <frg/cmdline.hpp>
#include <frg/scope_exit.hpp>
#include <frg/span.hpp>
#include <initgraph.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/cache.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/util.hpp>
#include <variant>

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
		UntranslatedOnly = 0b00,
		Passthrough = 0b10,
	};

	// low qword
	constexpr arch::field<uint64_t, bool> present{0, 1};
	constexpr arch::field<uint64_t, uint64_t> ssptptr{12, 52};
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

namespace qi {
	[[maybe_unused]] constexpr arch::field<uint64_t, uint8_t> type{0, 4};

	// VT-d specification rev 4.1, 6.5.2.1 Context-cache Invalidate Descriptor
	namespace context_cache_invalidate {
		enum class InvalidationGranularity : uint8_t {
			global = 0b01,
			domain = 0b10,
			device = 0b11,
		};

		[[maybe_unused]] constexpr arch::field<uint64_t, uint16_t> domainId{16, 16};
		[[maybe_unused]] constexpr arch::field<uint64_t, SourceID> sourceId{32, 16};
		[[maybe_unused]] constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{4, 2};
	} // namespace context_cache_invalidate

	// VT-d specification rev 4.1, 6.5.2.3 IOTLB Invalidate
	namespace iotlb_invalidate {
		enum class InvalidationGranularity : uint8_t {
			global = 0b01,
			domain = 0b10,
			page = 0b11,
		};

		[[maybe_unused]] constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{4, 2};
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> drainWrites{6, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> drainReads{7, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, uint16_t> domainId{16, 16};

		// high qword
		[[maybe_unused]] constexpr arch::field<uint64_t, uint8_t> addressMask{0, 6};
		[[maybe_unused]] constexpr arch::field<uint64_t, uint64_t> pageNumber{12, 52};
	} // namespace iotlb_invalidate

	// VT-d specification rev 4.1, 6.5.2.8 Invalidation Wait Descriptor
	namespace wait {
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> interruptFlag{4, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> statusWrite{5, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> fenceFlag{6, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, bool> pageRequestDrain{7, 1};
		[[maybe_unused]] constexpr arch::field<uint64_t, uint32_t> statusData{32, 32};
	} // namespace wait
} // namespace qi

namespace regs {
	constexpr arch::bit_register<uint32_t> version{0x00};
	constexpr arch::bit_register<uint64_t> capability{0x08};
	constexpr arch::bit_register<uint64_t> extendedCapability{0x10};
	constexpr arch::bit_register<uint32_t> globalCommand{0x18};
	constexpr arch::bit_register<uint32_t> globalStatus{0x1C};
	constexpr arch::scalar_register<uint64_t> rootEntryTable{0x20};
	[[maybe_unused]] constexpr arch::bit_register<uint64_t> contextCommand{0x28};
	constexpr arch::bit_register<uint32_t> faultStatus{0x34};
	constexpr arch::bit_register<uint32_t> faultEventControl{0x38};
	constexpr arch::scalar_register<uint32_t> faultEventData{0x3C};
	constexpr arch::scalar_register<uint32_t> faultEventAddress{0x40};
	constexpr arch::scalar_register<uint32_t> faultEventUpperAddress{0x44};
	constexpr arch::bit_register<uint32_t> protectedMemoryEnable{0x64};
	[[maybe_unused]] constexpr arch::scalar_register<uint64_t> invalidationQueueHead{0x80};
	[[maybe_unused]] constexpr arch::scalar_register<uint64_t> invalidationQueueTail{0x88};
	[[maybe_unused]] constexpr arch::bit_register<uint64_t> invalidationQueueAddress{0x90};
	[[maybe_unused]] constexpr arch::bit_register<uint32_t> invalidationCompletionStatus{0x9C};
	[[maybe_unused]] constexpr arch::bit_register<uint32_t> invalidationEventControl{0xA0};
	[[maybe_unused]] constexpr arch::scalar_register<uint32_t> invalidationEventData{0xA4};
	[[maybe_unused]] constexpr arch::scalar_register<uint32_t> invalidationEventAddress{0xA8};
	[[maybe_unused]] constexpr arch::scalar_register<uint32_t> invalidationEventUpperAddress{0xAC};
	[[maybe_unused]] constexpr arch::bit_register<uint64_t> invalidationQueueErrorRecord{0xB0};

	// IOTLB registers, to be used as offsets into the IOTLB mem_space
	[[maybe_unused]] constexpr arch::bit_register<uint64_t> iotlbInvalidateAddress{0x00};
	[[maybe_unused]] constexpr arch::bit_register<uint64_t> iotlbInvalidate{0x08};

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
	constexpr arch::field<uint64_t, bool> psi{39, 1};
	constexpr arch::field<uint64_t, uint8_t> nfr{40, 8};
	constexpr arch::field<uint64_t, uint8_t> maximumAddressMask{48, 6};
} // namespace capability

namespace extendedCapability {
	constexpr arch::field<uint64_t, bool> coherent{0, 1};
	constexpr arch::field<uint64_t, bool> queuedInvalidation{1, 1};
	constexpr arch::field<uint64_t, bool> pt{6, 1};
	constexpr arch::field<uint64_t, uint16_t> ivo{8, 10};
} // namespace extendedCapability

namespace globalStatus {
	constexpr arch::field<uint32_t, bool> interruptRemappingPointerStatus{24, 1};
	constexpr arch::field<uint32_t, bool> queuedInvalidationStatus{26, 1};
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

	[[maybe_unused]] constexpr arch::field<uint64_t, uint16_t> domainId{0, 16};
	[[maybe_unused]] constexpr arch::field<uint64_t, SourceID> sourceId{16, 16};
	[[maybe_unused]] constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{61, 2};
	[[maybe_unused]] constexpr arch::field<uint64_t, bool> invalidateContextCache{63, 1};
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

namespace invalidationCompletionStatus {
	[[maybe_unused]] constexpr arch::field<uint32_t, bool> complete{0, 1};
} // namespace invalidationCompletionStatus

namespace invalidationEventControl {
	[[maybe_unused]] constexpr arch::field<uint32_t, bool> interruptMask{31, 1};
	[[maybe_unused]] constexpr arch::field<uint32_t, bool> interruptPending{30, 1};
} // namespace invalidationEventControl

namespace invalidationQueue {
	enum class DescriptorWidth : uint8_t {
		Legacy128 = 0,
		Scalable256 = 1,
	};

	[[maybe_unused]] constexpr arch::field<uint64_t, uint8_t> queueSize{0, 3};
	[[maybe_unused]] constexpr arch::field<uint64_t, DescriptorWidth> descriptorWidth{11, 1};
	[[maybe_unused]] constexpr arch::field<uint64_t, uint64_t> queueBasePage{12, 52};
} // namespace invalidationQueue

namespace iotlbInvalidate {
	enum class InvalidationGranularity : uint8_t {
		Global = 0b001,
		Domain = 0b010,
		Page = 0b011,
	};

	[[maybe_unused]] constexpr arch::field<uint64_t, uint16_t> domainId{32, 16};
	[[maybe_unused]] constexpr arch::field<uint64_t, bool> drainWrites{48, 1};
	[[maybe_unused]] constexpr arch::field<uint64_t, bool> drainReads{49, 1};
	[[maybe_unused]] constexpr arch::field<uint64_t, InvalidationGranularity> invalidationGranularity{60, 3};
	[[maybe_unused]] constexpr arch::field<uint64_t, bool> invalidateIotlb{63, 1};
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
		case 0x01: return "Root Entry not present";
		case 0x02: return "Context Entry not present";
		case 0x03: return "Invalid Programming of Context Entry";
		case 0x04: return "Address overflow in second-stage translation";
		case 0x05: return "Write request failed with lack of permission";
		case 0x06: return "Read request failed with lack of permission";
		case 0x07: return "Next-level lookup invalid";
		case 0x08: return "Root-Table Address field invalid";
		case 0x09: return "Context-Table Pointer field invalid";
		case 0x0A: return "Non-zero reserved field in present root-entry";
		case 0x0B: return "Non-zero reserved field in present context-entry";
		case 0x0C: return "Non-zero reserved field in present second-stage PTE";
		default: return "Reserved or Unhandled";
	}
}

} // namespace

struct IntelIommuCursorPolicy {
private:
	size_t levels_;
	bool coherent_;

public:
	IntelIommuCursorPolicy(size_t levels, bool coherent) : levels_{levels}, coherent_{coherent} {}

	static inline constexpr size_t maxLevels = 5;
	static inline constexpr size_t bitsPerLevel = 9;

	static inline constexpr size_t iommuRead = 1 << 0;
	static inline constexpr size_t iommuWrite = 1 << 1;
	static inline constexpr size_t iommuIgnorePat = 1 << 6;
	static inline constexpr size_t iommuDirty = 1 << 9;
	// Mask for the physical address bits.
	static inline constexpr size_t iommuAddress = 0x000F'FFFF'FFFF'F000;

	inline constexpr size_t numLevels() {
		return levels_;
	}

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & iommuRead;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if(flags & page_access::read && !(pte & iommuRead))
			return false;

		if (flags & page_access::write && !(pte & iommuWrite))
			return false;

		if (flags & page_access::execute)
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & iommuAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(!ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		if(pte & iommuDirty)
			status |= page_status::dirty;
		return status;
	}

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~iommuDirty, __ATOMIC_RELAXED);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		if(cachingMode != CachingMode::null) {
			warningLogger() << frg::fmt("thor: request for unsupported VT-d caching mode {}; mapping as non-present", std::to_underlying(cachingMode)) << frg::endlog;
			return 0;
		}

		auto pte = physical | iommuIgnorePat | iommuRead; // TODO: Do not always set iommuRead.

		if(flags & page_access::write)
			pte |= iommuWrite;

		return pte;
	}

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool) {
		return {__atomic_load_n(ptePtr, __ATOMIC_RELAXED), false};
	}

	void pteWriteBarrier(uint64_t *ptePtr) {
		if (!coherent_)
			cacheFlush(ptePtr);
	}

	static constexpr void pteSyncICache(uintptr_t) { }

	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & iommuRead;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & iommuAddress;
	}

	uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);
		if (!coherent_)
			cacheFlush(accessor.get(), kPageSize);

		return newPtAddr | iommuRead | iommuWrite;
	}
};

using IntelIommuCursor = thor::PageCursor<IntelIommuCursorPolicy>;

struct IntelIommu;

struct IntelIommuOperations final : PageSpace, VirtualOperations {
	IntelIommuOperations(PhysicalAddr root, IntelIommu *iommu, uint16_t domainId)
	: PageSpace{root},
	  iommu_{iommu},
	  domainId_{domainId} {}

	void retire(RetireNode *node) override {
		// TODO: Invalidate IOTLB/context cache.
		node->complete();
	}

	bool submitShootdown(ShootNode *node) override;

	frg::expected<Error, PagesAffected> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> restrictPages(VirtualAddr va,
			size_t size, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size) override;

	frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size) override;

	frg::expected<Error, PagesAffected> agePages(VirtualAddr, size_t, bool) override {
		return PagesAffected{};
	}

	IntelIommu *iommu() const {
		return iommu_;
	}

	uint16_t domainId() const {
		return domainId_;
	}

private:
	IntelIommu *iommu_;
	uint16_t domainId_;
};

struct IntelIommuDmaSpace final : DmaSpace {
	IntelIommuDmaSpace(IntelIommuOperations *ops) : DmaSpace(ops), iommuOps_{ops} {}

	static smarter::shared_ptr<IntelIommuDmaSpace> create(IntelIommuOperations *ops) {
		auto ptr = smarter::allocate_shared<IntelIommuDmaSpace>(*kernelAlloc, ops);
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0x1000, (1UL << 39) - 0x1000);
		return ptr;
	}

	IntelIommuOperations *intelIommuOps() const {
		return iommuOps_;
	}

private:
	IntelIommuOperations *iommuOps_;
};

struct IntelIommuDomain : IommuDomain {
	IntelIommuDomain(uint16_t id, smarter::shared_ptr<IntelIommuDmaSpace> space)
	: IommuDomain(std::move(space)),
	  hwDid_{id} {}

	uint16_t hwDomainId() const { return hwDid_; }

private:
	uint16_t hwDid_;
};

struct IntelIommu final : Iommu, IrqSink {
	friend IntelIommuOperations;

	static constexpr size_t invalidationQueueSize = 256;

	IntelIommu(uint64_t register_base, uint16_t segment)
	: Iommu(nextIommuId++),
	IrqSink(frg::string(*kernelAlloc, "iommu") +
		frg::to_allocated_string(*kernelAlloc, id())),
	qi_{this},
	segment_{segment} {
		registerWindow_ = {register_base, 0x1000, CachingMode::mmio};
		regs_ = arch::mem_space{registerWindow_.get()};

		cap_ = regs_.load(regs::capability);
		ecap_ = regs_.load(regs::extendedCapability);

		auto sagaw = cap_ & capability::sagaw;
		assert(sagaw);
		if (sagaw & 2) { // bit 1: 39-bit AGAW (3-level paging)
			sagaw_ = 3;
		} else if (sagaw & 4) { // bit 2: 48-bit AGAW (4-level paging)
			sagaw_ = 4;
		} else {
			panicLogger() << "thor: IOMMU does not support 3-level or 4-level paging" << frg::endlog;
		}

		auto iotlbOffset = 16 * (ecap_ & extendedCapability::ivo);
		iotlbWindow_ = {register_base + iotlbOffset, 16, CachingMode::mmio};
		iotlb_ = arch::mem_space{iotlbWindow_.get()};

		auto faultRecordOffset = 16 * (cap_ & capability::fro);
		auto faultRecordCount = (cap_ & capability::nfr) + 1UL;
		faultRecordsWindow_ = {register_base + faultRecordOffset, faultRecordCount * 16, CachingMode::mmio};
		faultRecords_ = arch::mem_space{faultRecordsWindow_.get()};

		rootTablePhys_ = physicalAllocator->allocate(0x1000);
		if (rootTablePhys_ == static_cast<PhysicalAddr>(-1))
			panicLogger() << "thor: failed to allocate physical memory for IOMMU root table" << frg::endlog;
		PageAccessor rootTable{rootTablePhys_};
		memset(rootTable.get(), 0, 0x1000);
		flush(rootTable.get(), 0x1000);

		rootTable_ = frg::span{reinterpret_cast<rootTable::Entry *>(rootTable.get()), 0x1000 / sizeof(rootTable::Entry)};
	}

	struct InvalidationQueue : public IrqSink {
		InvalidationQueue(IntelIommu *iommu)
		: IrqSink(
		      frg::string(*kernelAlloc, iommu->name())
		      + frg::string<KernelAlloc>{*kernelAlloc, "-qi"}
		  ),
		  iommu_{iommu} {
			qiRingPhys_ = physicalAllocator->allocate(0x1000);
			if (qiRingPhys_ == static_cast<PhysicalAddr>(-1))
				panicLogger() << "thor: failed to allocate physical memory for IOMMU queued invalidation ring" << frg::endlog;
			PageAccessor qiRing{qiRingPhys_};
			memset(qiRing.get(), 0, 0x1000);
			qiStatusPhys_ = physicalAllocator->allocate(0x1000);
			if (qiStatusPhys_ == static_cast<PhysicalAddr>(-1))
				panicLogger() << "thor: failed to allocate physical memory for IOMMU queued invalidation status" << frg::endlog;
			PageAccessor qiStatus{qiStatusPhys_};
			memset(qiStatus.get(), 0, 0x1000);
			std::atomic_ref<uint32_t> statusWord{*reinterpret_cast<uint32_t *>(qiStatus.get())};
			statusWord.store(0xFFFFFFFF, std::memory_order_relaxed);
		}

		std::expected<void, thor::Error> init() {
			auto name = frg::string<KernelAlloc>{*kernelAlloc, "iommu"}
			            + frg::to_allocated_string(*kernelAlloc, iommu_->id())
			            + frg::string<KernelAlloc>{*kernelAlloc, "-invalidation-msi"};

			auto interrupt = allocateApicMsi(name);
			if (!interrupt)
				return std::unexpected{thor::Error::other};
			IrqPin::attachSink(interrupt, this);

			iommu_->regs_.store(regs::invalidationEventControl, invalidationEventControl::interruptMask(true));

			// Disable possibly pre-existing queued invalidation ring
			auto initialStatus = iommu_->regs_.load(regs::globalStatus);
			if (initialStatus & globalStatus::queuedInvalidationStatus) {
				auto status = initialStatus & arch::bit_mask(uint32_t(~globalStatus::clearedOnCompletion));
				iommu_->regs_.store(
				    regs::globalCommand,
				    status / globalStatus::queuedInvalidationStatus(false)
				);
			}
			while (iommu_->regs_.load(regs::globalStatus) & globalStatus::queuedInvalidationStatus)
				;

			// reset
			iommu_->regs_.store(regs::invalidationQueueTail, 0);
			iommu_->regs_.store(
			    regs::invalidationQueueAddress,
			    invalidationQueue::queueSize(0)
			        | invalidationQueue::descriptorWidth(
			            invalidationQueue::DescriptorWidth::Legacy128
			        )
			        | invalidationQueue::queueBasePage(qiRingPhys_ >> 12)
			);

			// enable queued invalidation
			iommu_->setGlobalBit(globalStatus::queuedInvalidationStatus(true));
			while (
			    !(iommu_->regs_.load(regs::globalStatus) & globalStatus::queuedInvalidationStatus)
			)
				;

			iommu_->regs_.store(regs::invalidationEventAddress, interrupt->getMessageAddress());
			iommu_->regs_.store(regs::invalidationEventUpperAddress, interrupt->getMessageAddress() >> 32);
			iommu_->regs_.store(regs::invalidationEventData, interrupt->getMessageData());

			iommu_->regs_.store(regs::invalidationCompletionStatus, invalidationCompletionStatus::complete(true));
			iommu_->regs_.store(regs::invalidationEventControl, invalidationEventControl::interruptMask(false));

			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), runSubmissionLoop());
			return {};
		}

	private:
		IrqStatus raise() override {
			if (iommu_->regs_.load(regs::invalidationCompletionStatus) & invalidationCompletionStatus::complete) {
				iommu_->regs_.store(regs::invalidationCompletionStatus, invalidationCompletionStatus::complete(true));

				PageAccessor statusPage{qiStatusPhys_};
				std::atomic_ref<uint32_t> statusWord{*reinterpret_cast<uint32_t *>(statusPage.get())};
				uint32_t completedSeq = statusWord.load(std::memory_order_relaxed);
				uint32_t head = qiHead_.load(std::memory_order_relaxed);

				if (completedSeq != head) {
					for (uint32_t seq = head + 1; seq != completedSeq + 1; seq++) {
						auto index = QueueIndex<invalidationQueueSize>{seq};
						auto a = completions_[index].exchange(nullptr, std::memory_order_acquire);
						if (a)
							a->complete();
					}
					qiHead_.store(completedSeq, std::memory_order_release);
					qiProgressEvent_.raise();
				}
			} else {
				return IrqStatus::nacked;
			}

			return IrqStatus::acked;
		}

		// must be called with `qiRingLock_` held.
		bool isCompleted(uint32_t seq) {
			uint32_t head = qiHead_.load(std::memory_order_acquire);
			return static_cast<int32_t>(head - seq) >= 0;
		}

		// must be called with `qiRingLock_` held.
		std::pair<QueueIndex<invalidationQueueSize>, uint32_t> allocateSeq() {
			uint32_t seq = qiTail_.load(std::memory_order_relaxed);
			qiTail_.store(seq + 1, std::memory_order_release);
			return {QueueIndex<invalidationQueueSize>{seq}, seq};
		}

		// must be called with `qiRingLock_` held.
		size_t freeSlots() {
			uint32_t head = qiHead_.load(std::memory_order_acquire);
			uint32_t tail = qiTail_.load(std::memory_order_relaxed);
			size_t used = tail - head;
			return invalidationQueueSize - 1 - used;
		}

		struct InvalidationRequest {
			enum class Type {
				globalContext,
				deviceContext,
				globalIotlb,
				domainIotlb,
				range
			} type;

			using Data = std::variant<std::monostate, SourceID, ShootNode *>;

			uint16_t domain;
			Data data;

			uint32_t sequence;
			bool submitted = false;
			bool completed = false;
			frg::default_list_hook<InvalidationRequest> hook;
		};

		template <typename F>
		coroutine<void> submitSyncInvalidation(InvalidationRequest::Type type, F func, uint16_t domain = 0, InvalidationRequest::Data data = std::monostate{}) {
			auto lock = frg::guard(&qiRingLock_);
			uint32_t seq = 0;

			if (pendingQueue_.empty() && freeSlots() >= 2) {
				PageAccessor qiRing{qiRingPhys_};
				frg::span<QiRingEntry128> qiRingEntries{reinterpret_cast<QiRingEntry128 *>(qiRing.get()), invalidationQueueSize};
				auto [index, _] = allocateSeq();
				auto &entry = qiRingEntries[index];

				func(entry);

				seq = submitSyncWait();
				lock.unlock();

				while (co_await qiProgressEvent_.async_wait_if([&] {
					auto irqLock = frg::guard(&qiRingLock_);
					return !isCompleted(seq);
				}))
					;
			} else {
				InvalidationRequest req;
				req.type = type;
				req.domain = domain;
				req.data = data;

				pendingQueue_.push_back(&req);
				pendingQueueEvent_.raise();
				lock.unlock();

				while (co_await qiProgressEvent_.async_wait_if([&] {
					auto irqLock = frg::guard(&qiRingLock_);
					if (!req.submitted)
						return true;
					return !isCompleted(req.sequence);
				}))
					;
			}
		}

	public:
		coroutine<void> invalidateGlobalContext() {
			return submitSyncInvalidation(
			    InvalidationRequest::Type::globalContext, [](auto &entry) {
				    entry.high = arch::bit_value<uint64_t>{0};
				    entry.low = qi::type(1);
				    entry.low |= qi::context_cache_invalidate::invalidationGranularity(
				        qi::context_cache_invalidate::InvalidationGranularity::global
				    );
			    }
			);
		}

		coroutine<void> invalidateDeviceContext(uint16_t domain, SourceID device) {
			return submitSyncInvalidation(
			    InvalidationRequest::Type::deviceContext, [&](auto &entry) {
				    entry.high = arch::bit_value<uint64_t>{0};
				    entry.low = qi::type(1);
				    entry.low |= qi::context_cache_invalidate::invalidationGranularity(
				        qi::context_cache_invalidate::InvalidationGranularity::device
				    );
				    entry.low |= qi::context_cache_invalidate::domainId(domain);
				    entry.low |= qi::context_cache_invalidate::sourceId(device);
			    }, domain, device
			);
		}

		coroutine<void> invalidateGlobalIotlb() {
			return submitSyncInvalidation(InvalidationRequest::Type::globalIotlb, [](auto &entry) {
				entry.high = arch::bit_value<uint64_t>{0};
				entry.low = qi::type(2);
				entry.low |= qi::iotlb_invalidate::invalidationGranularity(
				    qi::iotlb_invalidate::InvalidationGranularity::global
				);
				entry.low |= qi::iotlb_invalidate::drainWrites(true);
				entry.low |= qi::iotlb_invalidate::drainReads(true);
			});
		}

		coroutine<void> invalidateDomainIotlb(uint16_t domain) {
			return submitSyncInvalidation(
			    InvalidationRequest::Type::domainIotlb,
			    [&](auto &entry) {
				    entry.high = arch::bit_value<uint64_t>{0};
				    entry.low = qi::type(2);
				    entry.low |= qi::iotlb_invalidate::invalidationGranularity(
				        qi::iotlb_invalidate::InvalidationGranularity::domain
				    );
				    entry.low |= qi::iotlb_invalidate::drainWrites(true);
				    entry.low |= qi::iotlb_invalidate::drainReads(true);
				    entry.low |= qi::iotlb_invalidate::domainId(domain);
			    },
			    domain
			);
		}

		void invalidateRange(uint16_t domain, ShootNode *node) {
			auto [psi, required] = calculateRangeInvalidationParameters(node->address, node->size);
			auto lock = frg::guard(&qiRingLock_);

			if (pendingQueue_.empty() && freeSlots() >= required) {
				submitRange(domain, node, required, psi);
			} else {
				auto req = frg::construct<InvalidationRequest>(*kernelAlloc);
				req->type = InvalidationRequest::Type::range;
				req->domain = domain;
				req->data = node;

				pendingQueue_.push_back(req);
				pendingQueueEvent_.raise();
			}
		}

	private:
		// 128-bit QI descriptor
		struct QiRingEntry128 {
			arch::bit_value<uint64_t> low;
			arch::bit_value<uint64_t> high;
		};

		// must be called with `qiRingLock_` held.
		template <typename F>
		void submitAsync(F func) {
			auto [index, seq] = allocateSeq();
			PageAccessor qiRing{qiRingPhys_};
			frg::span<QiRingEntry128> qiRingEntries{reinterpret_cast<QiRingEntry128 *>(qiRing.get()), invalidationQueueSize};
			auto &entry = qiRingEntries[index];

			entry.low = qi::type(5);
			entry.low |= qi::wait::interruptFlag(true);
			entry.low |= qi::wait::statusWrite(true);
			entry.low |= qi::wait::statusData(seq);
			entry.low |= qi::wait::fenceFlag(true);
			entry.high = arch::bit_value<uint64_t>(qiStatusPhys_);

			func(index);

			iommu_->regs_.store(regs::invalidationQueueTail, (index + 1) << 4);
		}

		size_t submitSyncWait() {
			size_t seq = qiTail_.load(std::memory_order_relaxed);
			submitAsync([](size_t) {});
			return seq;
		}

		// Returns whether page-selective invalidation (PSI) is used (bool) and the number
		// of descriptors that the request takes (size_t).
		// Whether PSI is used for a request or not depends on controller capabilities,
		// invalidation address alignment and size.
		std::pair<bool, size_t> calculateRangeInvalidationParameters(uint64_t address, size_t size) {
			// If no controller support for PSI, invalidate the entire domain.
			if (!(iommu_->cap_ & capability::psi))
				return {false, 2};

			// Calculate the number of descriptors that PSI would take.
			size_t count = 0;
			auto length = size;
			auto addr = address;
			uint8_t maxAddressMask = iommu_->cap_ & capability::maximumAddressMask;
			size_t maxChunkPages = 1uz << maxAddressMask;
			size_t maxChunk = maxChunkPages << 12;

			while (length) {
				size_t alignment = (addr == 0) ? maxChunk : (addr & -addr);
				size_t largestFittingChunk = std::bit_floor(length);
				size_t chunk = std::min({alignment, largestFittingChunk, maxChunk});
				count++;
				addr += chunk;
				length -= chunk;
			}

			if (count + 1 < (invalidationQueueSize / 2))
				return {true, count + 1};

			// If we need more than half of the ring descriptors, fall back to invalidating
			// the entire domain instead. Filling up the ring buffer would be a DoS.
			return {false, 2};
		}

		// must be called with `qiRingLock_` held.
		void submitRange(uint16_t domain, ShootNode *node, size_t slots, bool psi) {
			assert((node->address & 0xFFF) == 0);

			PageAccessor qiRing{qiRingPhys_};
			frg::span<QiRingEntry128> qiRingEntries{reinterpret_cast<QiRingEntry128 *>(qiRing.get()), invalidationQueueSize};

			if (psi) {
				auto length = node->size;
				assert(length);
				assert((length & 0xFFF) == 0);

				auto addr = node->address;
				uint8_t maxAddressMask = iommu_->cap_ & capability::maximumAddressMask;
				size_t maxChunkPages = 1uz << maxAddressMask;
				size_t maxChunk = maxChunkPages << 12;

				while (length) {
					// Determine the natural alignment of this address.
					// (addr & -addr) isolates the lowest set bit, yielding the max power-of-2 alignment.
					size_t alignment = (addr == 0) ? maxChunk : (addr & -addr);
					size_t largestFittingChunk = std::bit_floor(length);
					size_t chunk = std::min({alignment, largestFittingChunk, maxChunk});
					assert(chunk);
					auto am = std::countr_zero(chunk) - 12;

					auto [index, seq] = allocateSeq();
					auto &entry = qiRingEntries[index];

					entry.high = qi::iotlb_invalidate::addressMask(am);
					entry.high |= qi::iotlb_invalidate::pageNumber(addr >> 12);
					entry.low = qi::type(2);
					entry.low |= qi::iotlb_invalidate::invalidationGranularity(qi::iotlb_invalidate::InvalidationGranularity::page);
					entry.low |= qi::iotlb_invalidate::drainWrites(true);
					entry.low |= qi::iotlb_invalidate::drainReads(true);
					entry.low |= qi::iotlb_invalidate::domainId(domain);

					addr += chunk;
					length -= chunk;
					slots--;
				}

				assert(slots == 1);
			} else {
				assert(slots == 2);

				auto [index, seq] = allocateSeq();
				auto &entry = qiRingEntries[index];

				entry.high = arch::bit_value<uint64_t>{0};
				entry.low = qi::type(2);
				entry.low |= qi::iotlb_invalidate::invalidationGranularity(qi::iotlb_invalidate::InvalidationGranularity::domain);
				entry.low |= qi::iotlb_invalidate::drainWrites(true);
				entry.low |= qi::iotlb_invalidate::drainReads(true);
				entry.low |= qi::iotlb_invalidate::domainId(domain);
			}

			submitAsync([&](size_t index) {
				completions_[index].store(node, std::memory_order_release);
			});
		}

		coroutine<void> runSubmissionLoop() {
			while (true) {
				while (co_await pendingQueueEvent_.async_wait_if([&] {
					auto irqLock = frg::guard(&qiRingLock_);
					return pendingQueue_.empty();
				}))
					;

				InvalidationRequest *req = nullptr;
				{
					auto irqLock = frg::guard(&qiRingLock_);
					assert(!pendingQueue_.empty());
					req = pendingQueue_.front();
					pendingQueue_.pop_front();
				}

				if (!req)
					continue;

				size_t slots = 0;
				bool psi = false;

				if (req->type == InvalidationRequest::Type::range) {
					auto node = std::get<ShootNode *>(req->data);
					std::tie(psi, slots) =
					    calculateRangeInvalidationParameters(node->address, node->size);
				} else {
					slots = 2;
				}

				while (true) {
					while (co_await qiProgressEvent_.async_wait_if([&] {
						auto irqLock = frg::guard(&qiRingLock_);
						return freeSlots() < slots;
					}))
						;

					auto irqLock = frg::guard(&qiRingLock_);
					if (freeSlots() >= slots) {
						uint32_t seq = 0;
						if (req->type == InvalidationRequest::Type::range) {
							submitRange(req->domain, std::get<ShootNode *>(req->data), slots, psi);
							frg::destruct(*kernelAlloc, req);
						} else {
							PageAccessor qiRing{qiRingPhys_};
							frg::span<QiRingEntry128> qiRingEntries{
							    reinterpret_cast<QiRingEntry128 *>(qiRing.get()),
							    invalidationQueueSize
							};
							auto [index, _] = allocateSeq();
							auto &entry = qiRingEntries[index];

							entry.high = arch::bit_value<uint64_t>{0};
							if (req->type == InvalidationRequest::Type::globalContext) {
								entry.low = qi::type(1);
								entry.low |= qi::context_cache_invalidate::invalidationGranularity(
								    qi::context_cache_invalidate::InvalidationGranularity::global
								);
							} else if (req->type == InvalidationRequest::Type::deviceContext) {
								entry.low = qi::type(1);
								entry.low |= qi::context_cache_invalidate::invalidationGranularity(
								    qi::context_cache_invalidate::InvalidationGranularity::device
								);
								entry.low |= qi::context_cache_invalidate::domainId(req->domain);
								entry.low |= qi::context_cache_invalidate::sourceId(
								    std::get<SourceID>(req->data)
								);
							} else if (req->type == InvalidationRequest::Type::globalIotlb) {
								entry.low = qi::type(2);
								entry.low |= qi::iotlb_invalidate::invalidationGranularity(
								    qi::iotlb_invalidate::InvalidationGranularity::global
								);
								entry.low |= qi::iotlb_invalidate::drainWrites(true);
								entry.low |= qi::iotlb_invalidate::drainReads(true);
							} else if (req->type == InvalidationRequest::Type::domainIotlb) {
								entry.low = qi::type(2);
								entry.low |= qi::iotlb_invalidate::invalidationGranularity(
								    qi::iotlb_invalidate::InvalidationGranularity::domain
								);
								entry.low |= qi::iotlb_invalidate::drainWrites(true);
								entry.low |= qi::iotlb_invalidate::drainReads(true);
								entry.low |= qi::iotlb_invalidate::domainId(req->domain);
							}
							seq = submitSyncWait();
							req->sequence = seq;
							req->submitted = true;
							qiProgressEvent_.raise();
						}
						break;
					}
				}
			}
		}

		IntelIommu *iommu_;

		IrqSpinlock qiRingLock_;
		PhysicalAddr qiRingPhys_;
		// Index of the next QI entry to be used.
		std::atomic<uint32_t> qiTail_{0};
		// The status page is used for the `Status Write` notifications for QI completion.
		PhysicalAddr qiStatusPhys_;

		// State tracking for the QI ring
		std::atomic<uint32_t> qiHead_{UINT32_MAX};
		async::recurring_event qiProgressEvent_;

		// Pending queue of requests waiting to be submitted to the QI ring
		async::recurring_event pendingQueueEvent_;

		using PendingQueue = frg::intrusive_list<
			InvalidationRequest,
			frg::locate_member<
				InvalidationRequest,
				frg::default_list_hook<InvalidationRequest>,
				&InvalidationRequest::hook
			>
		>;

		PendingQueue pendingQueue_;

		// Completion events to be fired
		std::array<std::atomic<ShootNode *>, invalidationQueueSize> completions_{};
	};

	coroutine<std::expected<void, thor::Error>> init() {
		co_await lock_.async_lock();
		frg::unique_lock logGuard{frg::adopt_lock, lock_};

		auto v = regs_.load(regs::version);
		infoLogger() << frg::fmt("thor: IOMMU version {}.{}",
				v & version::major, v & version::minor) << frg::endlog;
		infoLogger() << frg::fmt("thor: DRHD for segment {}", segment_) << frg::endlog;
		infoLogger() << frg::fmt("thor: cap 0x{:016x} ecap 0x{:016x}",
			uint64_t{cap_}, uint64_t{ecap_}) << frg::endlog;

		auto name = frg::string<KernelAlloc>{*kernelAlloc, "iommu"} +
			frg::to_allocated_string(*kernelAlloc, id()) +
			frg::string<KernelAlloc>{*kernelAlloc, "-fault-msi"};

		auto interrupt = allocateApicMsi(std::move(name));
		if (!interrupt)
			co_return std::unexpected{thor::Error::other};
		IrqPin::attachSink(interrupt, this);

		if (auto res = qi_.init(); !res)
			co_return res;

		// needs to be done before enabling translation
		writeBufferFlush();

		regs_.store(regs::faultEventControl, faultEventControl::interruptMask(true));

		regs_.store(regs::faultEventAddress, interrupt->getMessageAddress());
		regs_.store(regs::faultEventUpperAddress, interrupt->getMessageAddress() >> 32);
		regs_.store(regs::faultEventData, interrupt->getMessageData());

		regs_.store(regs::faultStatus, faultStatus::stickyBits);
		regs_.store(regs::faultEventControl, faultEventControl::interruptMask(false));

		co_await setRootEntryTable(rootTablePhys_);

		// sanitize firmware state by disabling this (optional) feature
		if(cap_ & capability::plmr || cap_ & capability::phmr) {
			regs_.store(regs::protectedMemoryEnable, regs_.load(regs::protectedMemoryEnable) / protectedMemoryEnable::epm(false));

			while(regs_.load(regs::protectedMemoryEnable) & protectedMemoryEnable::prs);
		}

		setGlobalBit(globalStatus::translationEnable(true));

		initialized_ = true;
		co_return {};
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

	bool pageWalkingCoherent() const {
		return ecap_ & extendedCapability::coherent;
	}

	bool supportsQueuedInvalidation() const {
		return ecap_ & extendedCapability::queuedInvalidation;
	}

	coroutine<void> enableDevice(pci::PciEntity *dev, bool passthrough) override {
		co_await lock_.async_lock();
		frg::unique_lock logGuard{frg::adopt_lock, lock_};

		auto rootEntry = &rootTable_[static_cast<uint8_t>(dev->bus)];
		PageAccessor context;
		frg::span<contextTable::Entry> contextTable{};

		if(!(rootEntry->entry.load() & rootTable::present)) {
			auto contextPhys = physicalAllocator->allocate(0x1000);
			if (contextPhys == static_cast<PhysicalAddr>(-1))
				panicLogger() << "thor: failed to allocate physical memory for IOMMU context table" << frg::endlog;
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
		bool oldPresent = contextEntry->low.load() & contextTable::present;
		uint16_t oldDomainId = oldPresent ? (contextEntry->high.load() & contextTable::domainId) : 0;

		int domainId = 1;

		if (passthrough) {
			contextEntry->high.store(
			    contextTable::addressWidth(sagaw_ - 2) | contextTable::domainId(1)
			);

			contextEntry->low.store(
			    contextTable::present(true)
			    | contextTable::translationType(contextTable::TranslationType::Passthrough)
			);
		} else {
			domainId = static_cast<IntelIommuDomain *>(dev->iommuDomain)->hwDomainId();
			contextEntry->high.store(
			    contextTable::addressWidth(sagaw_ - 2) | contextTable::domainId(domainId)
			);

			auto space = smarter::static_pointer_cast<IntelIommuDmaSpace>(dev->iommuDomain->space_);

			contextEntry->low.store(
			    contextTable::present(true)
			    | contextTable::ssptptr(space->intelIommuOps()->rootTable() >> 12)
			    | contextTable::translationType(contextTable::TranslationType::UntranslatedOnly)
			);
		}

		flush(contextEntry);

		bool do_invalidate = initialized_;
		logGuard.unlock();

		if(do_invalidate) {
			uint16_t invalidationDomainId = oldPresent ? oldDomainId : domainId;
			co_await qi_.invalidateDeviceContext(invalidationDomainId, sourceId);
			co_await qi_.invalidateDomainIotlb(domainId);
		}
	}

	uint8_t sagaw() const {
		return sagaw_;
	}

	uint16_t allocateHardwareDomainId() {
		return hwDidAlloc_++;
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

	coroutine<void> setRootEntryTable(uintptr_t physical) {
		assert((physical & 0xFFF) == 0);
		regs_.store(regs::rootEntryTable, physical);

		setGlobalBit(globalStatus::rootTablePointerStatus(true));

		co_await qi_.invalidateGlobalContext();
		co_await qi_.invalidateGlobalIotlb();
	}

	bool initialized_ = false;

	async::mutex lock_;

	PhysicalWindow registerWindow_;
	PhysicalWindow iotlbWindow_;
	PhysicalWindow faultRecordsWindow_;

	arch::mem_space regs_;
	arch::mem_space iotlb_;
	arch::mem_space faultRecords_;

	frg::span<rootTable::Entry> rootTable_;
	PhysicalAddr rootTablePhys_;

	InvalidationQueue qi_;

	uint16_t segment_;

	arch::bit_value<uint64_t> cap_ = arch::bit_value<uint64_t>{0};
	arch::bit_value<uint64_t> ecap_ = arch::bit_value<uint64_t>{0};

	// value for the Context Entry Address Width (AW) field for the highest supported page table level
	uint8_t sagaw_;

	uint16_t hwDidAlloc_ = 1;
};

bool IntelIommuOperations::submitShootdown(ShootNode *node) {
	iommu()->qi_.invalidateRange(domainId(), node);
	return false;
}

IntelIommu *handleDrhd(frg::span<uint8_t> remappingStructureTypes) {
	DmarDrhd drhd;
	memcpy(&drhd, remappingStructureTypes.data(), sizeof(drhd));

	auto iommu = frg::construct<IntelIommu>(*kernelAlloc, drhd.register_base, drhd.segment);

	if(!iommu->supportsPassthrough()) {
		infoLogger() << "thor: IOMMU does not support passthrough, ignoring" << frg::endlog;
		return nullptr;
	}

	if(!iommu->supportsQueuedInvalidation()) {
		infoLogger() << "thor: IOMMU does not support queued invalidations, ignoring" << frg::endlog;
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
						KernelFiber::asyncBlockCurrent(pciDev->associatedIommu->enableDevice(pciDev));
					} else {
						auto bridge = pciDev->parentBus->associatedBridge;

						while(bridge && bridge->parentBus && !bridge->associatedIommu)
							bridge = bridge->parentBus->associatedBridge;

						if(bridge && bridge->associatedIommu)
							KernelFiber::asyncBlockCurrent(bridge->associatedIommu->enableDevice(bridge));
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
						KernelFiber::asyncBlockCurrent(bridge->associatedIommu->enableDevice(bridge));
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

frg::expected<Error, PagesAffected> IntelIommuOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	IntelIommuCursorPolicy policy{iommu_->sagaw(), iommu_->pageWalkingCoherent()};
	return mapPresentPagesByCursor<IntelIommuCursor>(this, va, view, offset, size, flags, mode, policy);
}

frg::expected<Error, PagesAffected> IntelIommuOperations::restrictPages(VirtualAddr va,
		size_t size, PageFlags flags, CachingMode mode) {
	IntelIommuCursorPolicy policy{iommu_->sagaw(), iommu_->pageWalkingCoherent()};
	return restrictPagesByCursor<IntelIommuCursor>(this, va, size, flags, mode, policy);
}

frg::expected<Error, PagesAffected> IntelIommuOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) {
	IntelIommuCursorPolicy policy{iommu_->sagaw(), iommu_->pageWalkingCoherent()};
	return faultPageByCursor<IntelIommuCursor>(this, va, view, offset, fetchFlags, flags, mode, policy);
}

frg::expected<Error, PagesAffected> IntelIommuOperations::cleanPages(VirtualAddr va, size_t size) {
	IntelIommuCursorPolicy policy{iommu_->sagaw(), iommu_->pageWalkingCoherent()};
	return cleanPagesByCursor<IntelIommuCursor>(this, va, size, policy);
}

frg::expected<Error, PagesAffected> IntelIommuOperations::unmapPages(VirtualAddr va, size_t size) {
	IntelIommuCursorPolicy policy{iommu_->sagaw(), iommu_->pageWalkingCoherent()};
	return unmapPagesByCursor<IntelIommuCursor>(this, va, size, policy);
}

namespace {

IntelIommu *findIommu(pci::PciEntity *entity) {
	if (!entity)
		return nullptr;

	Iommu *iommu = nullptr;
	if (entity->type() == pci::PciEntityType::Device) {
		iommu = static_cast<pci::PciDevice *>(entity)->associatedIommu;
	} else if (entity->type() == pci::PciEntityType::Bridge) {
		iommu = static_cast<pci::PciBridge *>(entity)->associatedIommu;
	}

	if (iommu)
		return static_cast<IntelIommu *>(iommu);

	pci::PciBridge *bridge = entity->parentBus ? entity->parentBus->associatedBridge : nullptr;
	while (bridge && bridge->parentBus && !bridge->associatedIommu) {
		bridge = bridge->parentBus->associatedBridge;
	}

	if (bridge)
		return static_cast<IntelIommu *>(bridge->associatedIommu);

	return nullptr;
}

IommuDomain *newDomain(IntelIommu *iommu) {
	if (!iommu)
		return nullptr;

	PhysicalAddr rootLevel = physicalAllocator->allocate(kPageSize);
	if (rootLevel == static_cast<PhysicalAddr>(-1))
		panicLogger() << "thor: failed to allocate physical memory for IOMMU page tables" << frg::endlog;

	PageAccessor accessor{rootLevel};
	memset(accessor.get(), 0, kPageSize);

	auto domainId = iommu->allocateHardwareDomainId();

	auto ops = frg::construct<IntelIommuOperations>(*kernelAlloc, rootLevel, iommu, domainId);
	return frg::construct<IntelIommuDomain>(*kernelAlloc, domainId, IntelIommuDmaSpace::create(ops));
};

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
		frg::scope_exit finish {[&] {uacpi_table_unref(&dmarTbl);}};

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

		auto walkBus = [](this const auto &walkChildBus, pci::PciBus *bus, bool isRootBus = false) -> void {
			if (!bus)
				return;
			bool splitDeviceDomains = false;

			// Intel VT-d requires that RCIEs can be split into independent IOMMU domains.
			if (isRootBus) {
				splitDeviceDomains = true;
			} else if (bus->associatedBridge) {
				auto acs = std::ranges::find(bus->associatedBridge->extendedCaps, 0xD, &pci::PciEntity::ExtendedCapability::type);
				// TODO: check ACS caps and enable relevant bits
				if (acs != bus->associatedBridge->extendedCaps.end()) {
					// TODO: verify that ACS supports: Source Validation, Request/Completion Redirect, Upstream Forwarding
				}
			}

			IommuDomain *commonDomain = nullptr;
			if (!splitDeviceDomains && bus->associatedBridge)
				commonDomain = newDomain(findIommu(bus->associatedBridge));

			std::array<IommuDomain *, 32> multifunctionDomains{};

			for (auto device : bus->childDevices) {
				uint8_t header_type = bus->io->readConfigByte(bus, device->slot, 0, pci::kPciHeaderType);
				bool multifunctionDevice = header_type & 0x80;
				IommuDomain *domain = nullptr;

				if (multifunctionDevice) {
					if (multifunctionDomains[device->slot] == nullptr)
						multifunctionDomains[device->slot] = newDomain(findIommu(device));

					domain = multifunctionDomains[device->slot];
				} else if (splitDeviceDomains) {
					domain = newDomain(findIommu(device));
				} else {
					domain = commonDomain;
				}

				if (domain)
					domain->addMember(device);
			}

			for (auto bridge : bus->childBridges) {
				uint8_t header_type = bus->io->readConfigByte(bus, bridge->slot, bridge->function, pci::kPciHeaderType);
				bool multifunctionDevice = header_type & 0x80;

				IommuDomain *domain = nullptr;

				if (multifunctionDevice) {
					if (multifunctionDomains[bridge->slot] == nullptr)
						multifunctionDomains[bridge->slot] = newDomain(findIommu(bridge));

					domain = multifunctionDomains[bridge->slot];
				} else if (splitDeviceDomains) {
					domain = newDomain(findIommu(bridge));
				} else {
					domain = commonDomain;
				}

				if (domain)
					domain->addMember(bridge);

				if (bridge->associatedBus)
					walkChildBus(bridge->associatedBus);
			}
		};

		if (!iommus.empty()) {
			for (auto rootBus : std::ranges::subrange(pci::allRootBuses->begin(), pci::allRootBuses->end())) {
				walkBus(rootBus, true);
			}
		}

		for(auto iommu : iommus) {
			auto res = KernelFiber::asyncBlockCurrent(iommu->init());
			if (!res)
				warningLogger() << frg::fmt("thor: VT-d IOMMU {} failed to init, ignoring it", iommu->id());
		}
	}
};

}
