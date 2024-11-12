#include <nic/pcnet/pcnet.hpp>
#include <arch/dma_pool.hpp>
#include <arch/dma_structs.hpp>
#ifdef __x86_64__
#include <arch/io_space.hpp>
#endif
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <arch/variable.hpp>
#include <async/recurring-event.hpp>
#include <async/oneshot-event.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>
#include <netserver/nic.hpp>
#include <core/queue.hpp>
#include <queue>
#include <string.h>

// debug options
namespace {

constexpr bool logDriverStuff = false;

}

namespace {

// AMD PCNET has two register sets, indexed by the same RAP32/RAP16 register
// - RDP is for CSRs
// - BDP is for BCRs
//
// Simple as that!
inline constexpr arch::scalar_register<uint32_t> PCNET_MAC_1(0x0);
inline constexpr arch::scalar_register<uint32_t> PCNET_MAC_2(0x4);

inline constexpr arch::scalar_register<uint16_t> PCNET16_RDP(0x10);
inline constexpr arch::scalar_register<uint16_t> PCNET16_RAP(0x12);
inline constexpr arch::scalar_register<uint16_t> PCNET16_RST(0x14);
inline constexpr arch::scalar_register<uint16_t> PCNET16_BDP(0x16);

inline constexpr arch::scalar_register<uint32_t> PCNET32_RDP(0x10);
inline constexpr arch::scalar_register<uint32_t> PCNET32_RAP(0x14);
inline constexpr arch::scalar_register<uint32_t> PCNET32_RST(0x18);
inline constexpr arch::scalar_register<uint32_t> PCNET32_BDP(0x1c);

inline constexpr uint32_t MAX_FRAME_SIZE = 1520;

struct Request {
	Request(size_t size) : index(0, size) { };
	QueueIndex index;
	async::oneshot_event event;
	arch::dma_buffer_view frame;
};

struct [[gnu::packed]] Descriptor {
	uint32_t addr;
	uint16_t length;
	uint16_t status;
	uint32_t msg_length;
	uint32_t res;
};
static_assert(sizeof(Descriptor) == 16);

struct [[gnu::packed]] InitializerDescriptor {
	uint16_t mode;
	uint8_t rx_len;
	uint8_t tx_len;
	//4
	uint8_t mac[6];
	uint16_t reserved_0;
	//12
	uint8_t ladr[8];
	//20
	uint32_t rx_paddr;
	//24
	uint32_t tx_paddr;
};
static_assert(sizeof(InitializerDescriptor) == 28);

struct PcNetNic;

template<bool IsTransmit>
struct PcNetQueue {
	static const uint32_t descriptor_count = IsTransmit ? 8 : 32;

	std::vector<std::shared_ptr<Request>> requests;
	arch::dma_array<Descriptor> descriptors;
	QueueIndex next_index;
	std::vector<arch::dma_buffer> buffers;

	PcNetQueue() : next_index(0, descriptor_count) {

	}
	void setLength(uint32_t i, uint32_t len) {
		descriptors[i].length = uint16_t((-len) & 0x0fff) | 0xf000;
	}
	void init(PcNetNic& nic);
};

struct PcNetNic : nic::Link {
	PcNetNic(protocols::hw::Device dev);
	virtual async::result<size_t> receive(arch::dma_buffer_view);
	virtual async::result<void> send(const arch::dma_buffer_view);
	virtual ~PcNetNic() = default;

	async::result<void> init();
	async::detached processIrqs();
protected:
	async::result<uint32_t> pollDevice();
	uint32_t csr_read(uint32_t n);
	void csr_write(uint32_t n, uint32_t m);
	uint32_t bcr_read(uint32_t n);
	void bcr_write(uint32_t n, uint32_t m);
	void print_status();

	arch::contiguous_pool dmaPool_;
	protocols::hw::Device device_;
	helix::UniqueDescriptor irq_;
	helix::Mapping mmio_mapping_;
	arch::mem_space mmio_;
	arch::dma_buffer initializer_;
	PcNetQueue<true> tx_;
	PcNetQueue<false> rx_;
};

template<bool IsTransmit>
void PcNetQueue<IsTransmit>::init(PcNetNic& nic) {
	descriptors = arch::dma_array<Descriptor>(nic.dmaPool(), descriptor_count);
	if(logDriverStuff)
		std::cout << "drivers/pcnet: setting " << descriptor_count << " buffers at " << (void*)helix_ng::ptrToPhysical(descriptors.data()) << " of " << (IsTransmit ? "TX" : "RX") << std::endl;
	
	for(uint32_t i = 0; i < descriptor_count; ++i) {
		auto buf = arch::dma_buffer(nic.dmaPool(), MAX_FRAME_SIZE);
		memset(buf.data(), 0, buf.size());
		uintptr_t addr = helix_ng::ptrToPhysical(buf.data());
		uint16_t len = uint16_t(buf.size());
		//
		descriptors[i].addr = uint32_t(addr);
		this->setLength(i, len);
		if constexpr(IsTransmit) {
			descriptors[i].status = 0;
		} else {
			descriptors[i].status = 0x8000;
		}
		descriptors[i].msg_length = 0;
		descriptors[i].res = 0;
		//
		if(logDriverStuff)
			std::cout << "drivers/pcnet: setup@buffer " << (void*)addr << " size " << buf.size() << std::endl;
		
		buffers.push_back(std::move(buf));
	}
}

uint32_t PcNetNic::csr_read(uint32_t n) {
	mmio_.store(PCNET32_RAP, n); //read
	return mmio_.load(PCNET32_RDP);
}
void PcNetNic::csr_write(uint32_t n, uint32_t m) {
	mmio_.store(PCNET32_RAP, n); //write
	mmio_.store(PCNET32_RDP, m);
}

uint32_t PcNetNic::bcr_read(uint32_t n) {
	mmio_.store(PCNET32_RAP, n); //read
	return mmio_.load(PCNET32_BDP);
}
void PcNetNic::bcr_write(uint32_t n, uint32_t m) {
	mmio_.store(PCNET32_RAP, n); //write
	mmio_.store(PCNET32_BDP, m);
}

async::result<void> PcNetNic::init() {
	// Setup for PCI - then select the first MMIO bar
	irq_ = co_await device_.accessIrq();
	co_await device_.enableBusmaster();
	auto info = co_await device_.getPciInfo();
	size_t bar_index = 0;
	for(size_t bar_index = 0; bar_index < 6; ++bar_index) {
		if(info.barInfo[bar_index].ioType == protocols::hw::IoType::kIoTypeMemory)
			break;
#if __x86_64__
		if(info.barInfo[bar_index].ioType == protocols::hw::IoType::kIoTypePort)
			break;
#endif
		assert(bar_index >= 6 && "drivers/pcnet: unable to locate MMIO BAR!");
	}

	if(logDriverStuff)
		std::cout << "drivers/pcnet: selected pci bar " << bar_index << std::endl;

	auto &barInfo = info.barInfo[bar_index];
	assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await device_.accessBar(bar_index);
	mmio_mapping_ = {bar, barInfo.offset, barInfo.length};
	mmio_ = mmio_mapping_.get();

	// Now we are ready to start sending
	// Reset the device (in both modes, 16-bit and 32-bit)
	// Cleverly, RST is on +0x18 for 32-bit mode, so reading from there causes
	// no (important) side effects
	mmio_.load(PCNET32_RST);
	mmio_.load(PCNET16_RST);
	co_await helix::sleepFor(1'000'000);
	mmio_.store(PCNET32_RDP, 0); // 32-bit mode

	// SWSTYLE read and writeback to csr 58
	csr_write(58, (csr_read(58) & 0xff00) | 0x02);
	// ASEL will automatically select coaxial/fiber/etc
	bcr_write(2, bcr_read(2) | 0x02);

	auto mac_lower = mmio_.load(PCNET_MAC_1);
	auto mac_higher = mmio_.load(PCNET_MAC_2);
	mac_[0] = (mac_lower >> 0) & 0xFF;
	mac_[1] = (mac_lower >> 8) & 0xFF;
	mac_[2] = (mac_lower >> 16) & 0xFF;
	mac_[3] = (mac_lower >> 24) & 0xFF;
	mac_[4] = (mac_higher >> 0) & 0xFF;
	mac_[5] = (mac_higher >> 8) & 0xFF;
	if(logDriverStuff)
		std::cout << "drivers/pcnet: MAC " << mac_ << std::endl;

	//
	// TRANSMIT
	tx_.init(*this);

	//
	// RECEIVE
	rx_.init(*this);

	//
	// INITIALIZE
	initializer_ = arch::dma_buffer(dmaPool(), sizeof(InitializerDescriptor));
	auto *init = (InitializerDescriptor*)initializer_.data();
	init->mode = 0;
	init->reserved_0 = 0;
	std::fill(begin(init->ladr), end(init->ladr), 0);
	memcpy(&init->mac[0], &mac_[0], sizeof(mac_));
	//
	init->rx_len = 5 << 4;
	init->tx_len = 3 << 4;
	//
	init->tx_paddr = helix_ng::ptrToPhysical(tx_.descriptors.data());
	init->rx_paddr = helix_ng::ptrToPhysical(rx_.descriptors.data());

	// CSR1 and CSR2 hold the initialization structure!
	uintptr_t init_addr = helix_ng::ptrToPhysical(initializer_.data());
	mmio_.store(PCNET32_RAP, 1); //write
	mmio_.store(PCNET32_RDP, uint16_t(init_addr));
	mmio_.store(PCNET32_RAP, 2); //write
	mmio_.store(PCNET32_RDP, uint16_t(init_addr >> 16));

	//
	// Unset bits 10,9,8 so we get RX,TX and INIT irqs (respectively)
	if(logDriverStuff)
		std::cout << "drivers/pcnet: step-unset-irqs" << std::endl;
	auto csr3 = csr_read(3) & ~((1 << 12) | (1 << 11) | (1 << 10) | (1 << 9) | (1 << 8));
	csr_write(3, csr3);

	//
	// Automatically pad ethernet packets
	if(logDriverStuff)
		std::cout << "drivers/pcnet: step-automatically-pad-ethernet-packets" << std::endl;
	auto csr4 = csr_read(4) | (1 << 11);
	csr_write(4, csr4);

	//
	// Initialize step
	if(logDriverStuff)
		std::cout << "drivers/pcnet: step-initialize" << std::endl;
	csr_write(0, (1 << 0) | (1 << 6));

	processIrqs();
}

PcNetNic::PcNetNic(protocols::hw::Device device)
	: nic::Link(1500, &dmaPool_), device_ { std::move(device) }
{
	//promiscuous_ = true;
	//all_multicast_ = true;
	//multicast_ = true;
	//broadcast_ = true;
	//l1_up_ = true;
	async::run(this->init(), helix::currentDispatcher);
}

async::result<uint32_t> PcNetNic::pollDevice() {
	co_return mmio_.load(PCNET32_RDP);
}

async::result<size_t> PcNetNic::receive(arch::dma_buffer_view frame) {
	if(logDriverStuff)
		std::cout << "drivers/pcnet: receive() -> " << frame.size() << std::endl;
	
	auto req = std::make_shared<Request>(rx_.descriptor_count);
	req->frame = frame;
	req->index = rx_.next_index;
	assert(rx_.buffers[req->index].size() > frame.size());
	memset(rx_.buffers[req->index].data(), 0, frame.size());
	rx_.requests.push_back(req);
	++rx_.next_index;
	co_await req->event.wait();
	co_return frame.size();
}

async::result<void> PcNetNic::send(const arch::dma_buffer_view frame) {
	if(logDriverStuff)
		std::cout << "drivers/pcnet: send() -> " << frame.size() << std::endl;

	auto req = std::make_shared<Request>(tx_.descriptor_count);
	req->frame = frame;
	req->index = tx_.next_index;
	assert(tx_.buffers[req->index].size() > frame.size());
	tx_.setLength(req->index, frame.size());
	memcpy(tx_.buffers[req->index].data(), frame.data(), frame.size()); //nov the pcnet will steal our dma
	tx_.descriptors[req->index].status |= 0x0200; //START OF SPLIT PACKET
	tx_.descriptors[req->index].status |= 0x0100; //END OF PACKET
	tx_.descriptors[req->index].status |= 0x8000; //give it to the device!
	__sync_synchronize();
	tx_.requests.push_back(req);
	++tx_.next_index;
	co_await req->event.wait();
}

async::detached PcNetNic::processIrqs() {
	co_await device_.enableBusIrq();
	if(logDriverStuff)
		std::cout << "drivers/pcnet: IRQs enabled!" << std::endl;

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckKick, 0));
	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(irq_, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		__sync_synchronize();

		if(logDriverStuff)
			std::cout << "drivers/pcnet: Got IRQ #" << sequence << "!!!" << std::endl;

		auto const csr0 = csr_read(0);
		uint32_t new_csr0 = 0;
		// Handle receives
		if((csr0 & (1 << 10)) != 0) { // RINT -- completed receive frame
			if(logDriverStuff)
				std::cout << "drivers/pcnet: IRQ-RINT" << std::endl;
			
			for(uint32_t i = 0; i < rx_.requests.size(); i++) {
				auto req = rx_.requests[i];
				if((rx_.descriptors[req->index].status & 0x8000) != 0) //owned by card
					continue;
				if(logDriverStuff)
					std::cout << "drivers/pcnet: RX request @ " << req->index << "!!!" << std::endl;

				auto size = rx_.descriptors[req->index].msg_length;
				memcpy(req->frame.data(), rx_.buffers[req->index].data(), size); //steal - erm... "borrow" the data
				req->frame = req->frame.subview(0, size);
				rx_.descriptors[req->index].status = 0x8000; //give device back the buffer
				req->event.raise();
				rx_.requests.erase(rx_.requests.begin() + i);
			}
			new_csr0 |= (1 << 10); //mask off RINT
		}
		// Handle transmits
		if((csr0 & (1 << 9)) != 0) { // TINT -- completed transmit frame
			if(logDriverStuff)
				std::cout << "drivers/pcnet: IRQ-TINT" << std::endl;
			
			for(uint32_t i = 0; i < tx_.requests.size(); i++) {
				auto req = tx_.requests[i];
				if((tx_.descriptors[req->index].status & 0x8000) != 0) //still owned by card
					continue;
				if(logDriverStuff)
					std::cout << "drivers/pcnet: TX request @ " << req->index << "!!!" << std::endl;
				
				req->event.raise();
				tx_.requests.erase(tx_.requests.begin() + i);
			}
			new_csr0 |= (1 << 9); //mask off TINT
		}
		if((csr0 & (1 << 8)) != 0) { //IDON -- initialization complete
			// Start da card!
			if(logDriverStuff)
				std::cout << "drivers/pcnet: IRQ-IDON" << std::endl;
			
			new_csr0 &= ~((1 << 0) | (1 << 2)); //clear stop and init bit
			new_csr0 |= (1 << 1); //set start bit
			new_csr0 |= (1 << 8); //mask off IDON
		}
		new_csr0 |= (1 << 6);
		csr_write(0, new_csr0);

		if(new_csr0 != csr0 && logDriverStuff)
			std::cout << "drivers/pcnet: CSR0(old)" << (void*)(uintptr_t)csr0 << " != (new)" << (void*)(uintptr_t)new_csr0 << " !!!" << std::endl;
		
		if(logDriverStuff)
			print_status();

		HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, sequence));
	}
}

void print_status() {
	assert(logDriverStuff) {
	auto const ct = csr_read(0);
	std::cout << "drivers/pcnet: INIT? " << ((ct & (1 << 0)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: STRT? " << ((ct & (1 << 1)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: STOP? " << ((ct & (1 << 2)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: TDMD? " << ((ct & (1 << 3)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: TXON? " << ((ct & (1 << 4)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: RXON? " << ((ct & (1 << 5)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: ITNT? " << ((ct & (1 << 6)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/amdpcent: === INTERRUPTS ===" << std::endl;
	std::cout << "drivers/pcnet: INTR? " << ((ct & (1 << 7)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: IDON? " << ((ct & (1 << 8)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: TINT? " << ((ct & (1 << 9)) != 0 ? "YES" : "NO") << std::endl;
	std::cout << "drivers/pcnet: RINT? " << ((ct & (1 << 10)) != 0 ? "YES" : "NO") << std::endl;
}

} // namespace

namespace nic::pcnet {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device) {
	return std::make_shared<PcNetNic>(std::move(device));
}

} // namespace nic::pcnet
