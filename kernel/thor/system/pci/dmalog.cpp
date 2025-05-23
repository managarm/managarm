#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/physical.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <async/sequenced-event.hpp>

namespace thor::pci {

namespace {

struct DmalogSglist {
	uint64_t ptr;
	uint64_t length;
};

struct DmalogDescriptor {
	uint64_t status = {};
	uint64_t actualLength = {};
	uint64_t flags = {};
	uint64_t numBuffers = {};
	DmalogSglist buffers[];
};

constexpr arch::scalar_register<uint64_t> outRegister{0x0};
constexpr arch::scalar_register<uint64_t> inRegister{0x8};
constexpr arch::bit_register<uint32_t> isrRegister{0x10};

constexpr arch::field<uint32_t, bool> isrOutStatus{0, 1};
constexpr arch::field<uint32_t, bool> isrInStatus{1, 1};

struct DmalogDevice final : IrqSink, KernelIoChannel {
	static constexpr size_t ringSize = kPageSize;

	DmalogDevice(frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag, void *mmioPtr)
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "dmalog-"}
				+ tag
				+ frg::string<KernelAlloc>{*kernelAlloc, "-irq"}},
			KernelIoChannel{std::move(tag), std::move(descriptiveTag)},
			mmioSpace_{mmioPtr} {
		ctrlPhysical_ = physicalAllocator->allocate(kPageSize);
		outPhysical_ = physicalAllocator->allocate(kPageSize);
		inPhysical_ = physicalAllocator->allocate(kPageSize);
		assert(ctrlPhysical_ != PhysicalAddr(-1) && "OOM in dmalog");
		assert(outPhysical_ != PhysicalAddr(-1) && "OOM in dmalog");
		assert(inPhysical_ != PhysicalAddr(-1) && "OOM in dmalog");

		// Map the output/input ring buffers twice such users can always see the available
		// part of the buffer in one (virtually) contiguous memory range.
		outView_ = reinterpret_cast<std::byte *>(
				KernelVirtualMemory::global().allocate(2 * ringSize));
		inView_ = reinterpret_cast<std::byte *>(
				KernelVirtualMemory::global().allocate(2 * ringSize));

		// TODO: We need to map more memory if we want to support rings > kPageSize.
		KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(outView_),
				outPhysical_, page_access::write, CachingMode::writeBack);
		KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(outView_) + kPageSize,
				outPhysical_, page_access::write, CachingMode::writeBack);

		// TODO: We need to map more memory if we want to support rings > kPageSize.
		KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(inView_),
				inPhysical_, page_access::write, CachingMode::writeBack);
		KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(inView_) + kPageSize,
				inPhysical_, page_access::write, CachingMode::writeBack);

		PageAccessor ctrlAccessor{ctrlPhysical_};
		auto ctrlPtr = reinterpret_cast<std::byte *>(ctrlAccessor.get());
		outDesc_ = new (ctrlPtr) DmalogDescriptor{};
		inDesc_ = new (ctrlPtr + 2048) DmalogDescriptor{};

		// Initially, both buffers are empty.
		updateWritableSpan({outView_, ringSize});
		updateReadableSpan({inView_, 0});
	}

	void produceOutput(size_t n) override {
		assert(outHead_ >= outTail_);
		assert(outHead_ + n <= outTail_ + ringSize);

		outHead_ += n;
		updateWritableSpan({outView_ + (outHead_ & (ringSize - 1)),
				ringSize - (outHead_ - outTail_)});
	}

	void consumeInput(size_t n) override {
		assert(inHead_ >= inTail_ + n);
		assert(inHead_ <= inTail_ + ringSize);

		inTail_ += n;
		updateReadableSpan({inView_ + (inTail_ & (ringSize - 1)), inHead_ - inTail_});
	}

	coroutine<frg::expected<Error>> issueIo(IoFlags flags) override {
		assert(outHead_ >= outTail_);
		assert(outHead_ <= outTail_ + ringSize);
		assert(inHead_ >= inTail_);
		assert(inHead_ <= inTail_ + ringSize);

		if(!outPending_ && (flags & ioProgressOutput)) {
			auto size = outHead_ - outTail_;
			if(!size)
				co_return Error::illegalState;

			*outDesc_ = {
				.flags = 1
			};

			auto offset = outTail_ & (ringSize - 1);

			size_t progress = 0;
			size_t k = 0;
			while(progress < size) {
				assert(k < 2);
				auto misalign = (offset + progress) & (kPageSize - 1);
				auto chunk = frg::min(size - progress, kPageSize - misalign);

				outDesc_->buffers[k] = {
					.ptr = outPhysical_ + misalign,
					.length = chunk
				};
				progress += chunk;
				++k;
			}
			outDesc_->numBuffers = k;

			mmioSpace_.store(outRegister, ctrlPhysical_);
			outPending_ = true;
		}

		if(!inPending_ && (flags & ioProgressInput)) {
			auto size = ringSize - (inHead_ - inTail_);
			if(!size)
				co_return Error::illegalState;

			*inDesc_ = {
				.flags = 1
			};

			auto offset = inHead_ & (ringSize - 1);

			size_t progress = 0;
			size_t k = 0;
			while(progress < size) {
				assert(k < 2);
				auto misalign = (offset + progress) & (kPageSize - 1);
				auto chunk = frg::min(size - progress, kPageSize - misalign);

				inDesc_->buffers[k] = {
					.ptr = inPhysical_ + misalign,
					.length = chunk
				};
				progress += chunk;
				++k;
			}
			inDesc_->numBuffers = k;

			mmioSpace_.store(inRegister, ctrlPhysical_ + 2048);
			inPending_ = true;
		}

		// Potentially wait for an IRQ.
		if(!outPending_ && !inPending_)
			co_return {};

		bool inIrq = false, outIrq = false;
		while (true) {
			irqSeq_ = co_await irqEvent_.async_wait(irqSeq_);

			// Schedule on the work queue in order to return from the IRQ handler
			co_await WorkQueue::generalQueue()->schedule();

			auto inSeq = inSeq_.load(std::memory_order_acquire);
			auto outSeq = outSeq_.load(std::memory_order_acquire);

			inIrq = inIrq || (inSeq == irqSeq_);
			outIrq = outIrq || (outSeq == irqSeq_);

			if (inIrq && (flags & ioProgressInput))
				break;

			if (outIrq && (flags & ioProgressOutput))
				break;
		}

		// Process output/input.

		if(outIrq) {
			assert(outDesc_->status);
			assert(outDesc_->actualLength);
			outTail_ += outDesc_->actualLength;

			assert(outTail_ == outHead_);
			updateWritableSpan({outView_ + (outHead_ & (ringSize - 1)), ringSize});
			outPending_ = false;
		}

		if(inIrq) {
			assert(inDesc_->status);
			assert(inDesc_->actualLength);
			inHead_ += inDesc_->actualLength;

			updateReadableSpan({inView_ + (inTail_ & (ringSize - 1)), inHead_ - inTail_});
			inPending_ = false;
		}

		co_return {};
	}

	IrqStatus raise() override {
		auto isrBits = mmioSpace_.load(isrRegister);
		bool outIrq = isrBits & isrOutStatus;
		bool inIrq = isrBits & isrInStatus;

		if (outIrq || inIrq) {
			// Clear the ISR.
			mmioSpace_.store(isrRegister, isrOutStatus(outIrq) | isrInStatus(inIrq));
			auto seq = irqEvent_.next_sequence();

			if (outIrq)
				outSeq_.store(seq, std::memory_order_release);
			if (inIrq)
				inSeq_.store(seq, std::memory_order_release);

			irqEvent_.raise();

			return IrqStatus::acked;
		} else {
			return IrqStatus::nacked;
		}
	}

private:
	arch::mem_space mmioSpace_;
	smarter::shared_ptr<IrqObject> irqObject_;
	PhysicalAddr ctrlPhysical_;
	PhysicalAddr outPhysical_;
	PhysicalAddr inPhysical_;
	DmalogDescriptor *outDesc_, *inDesc_;
	std::byte *outView_;
	const std::byte *inView_;

	uint64_t outTail_ = 0, outHead_ = 0;
	uint64_t inTail_ = 0, inHead_ = 0;
	uint64_t irqSeq_ = 0;
	std::atomic_uint64_t inSeq_ = 0, outSeq_ = 0;
	async::sequenced_event irqEvent_;
	bool outPending_ = false, inPending_ = false;
};

static initgraph::Task enumerateDmalog{&globalInitEngine, "pci.enumerate-dmalog",
	initgraph::Requires{getDevicesEnumeratedStage()},
	initgraph::Entails{getIoChannelsDiscoveredStage()},
	[] {
		for(smarter::shared_ptr<PciDevice> pciDevice : *allDevices) {
			if(pciDevice->vendor != 0x1234
					|| pciDevice->deviceId != 0x69e8
					|| pciDevice->revision != 0x12)
				continue;

			auto mmioPtr = KernelVirtualMemory::global().allocate(0x10000);
			KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(mmioPtr),
					pciDevice->bars[0].address, page_access::write, CachingMode::null);

			char tag[64]{};
			size_t n;
			auto tagSpace = arch::mem_space{mmioPtr}.subspace(0x40);
			for(n = 0; n < 64; ++n) {
				auto c = tagSpace.load(arch::scalar_register<uint8_t>(n));
				if(!c)
					break;
				tag[n] = c;
			}
			infoLogger() << "thor: Found PCI-based dmalog at "
					<< pciDevice->bus << ":" << pciDevice->slot
					<< ", tag: " << tag << frg::endlog;

			auto dmalog = smarter::allocate_shared<DmalogDevice>(*kernelAlloc,
					frg::string<KernelAlloc>{*kernelAlloc, tag},
					frg::string<KernelAlloc>{*kernelAlloc, tag},
					mmioPtr);

			bool useMsi = false;

			if(pciDevice->numMsis) {
				auto pin = pciDevice->parentBus->msiController->allocateMsiPin(
					frg::string<KernelAlloc>{*kernelAlloc, "pci-msi."}
					+ frg::to_allocated_string(*kernelAlloc, pciDevice->bus)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, pciDevice->slot)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, pciDevice->function)
					+ frg::string<KernelAlloc>{*kernelAlloc, ".0"});
				if(!pin) {
					warningLogger() << "thor: could not allocate MSI for dmalog" << frg::endlog;
				} else {
					IrqPin::attachSink(pin, dmalog.get());

					pciDevice->enableBusmaster();
					pciDevice->setupMsi(pin, 0);
					pciDevice->enableMsi();
					useMsi = true;
				}
			}

			// fall back to legacy IRQs if necessary
			if(!useMsi) {
				IrqPin::attachSink(pciDevice->getIrqPin(), dmalog.get());
				pciDevice->enableIrq();
			}

			publishIoChannel(std::move(dmalog));
		}
	}
};

} // anonymous namespace

} // namespace thor::pci
