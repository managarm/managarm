
#include <stdio.h>
#include <assert.h>

#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <bragi/mbus.hpp>
#include <hw.pb.h>

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

// Alignment makes sure that a packet doesnt cross a page boundary
struct alignas(8) SetupPacket {
	enum DataDirection {
		kDirToDevice = 0,
		kDirToHost = 1
	};

	enum Recipient {
		kDestDevice = 0,
		kDestInterface = 1,
		kDestEndpoint = 2,
		kDestOther = 3
	};

	enum Type {
		kStandard = 0,
		kClass = 1,
		kVendor = 2,
		kReserved = 3
	};

	static constexpr uint8_t RecipientBits = 0;
	static constexpr uint8_t TypeBits = 5;
	static constexpr uint8_t DirectionBit = 7;

	SetupPacket(DataDirection data_direction, Recipient recipient, Type type,
			uint8_t breq, uint16_t wval, uint16_t wid, uint16_t wlen)
	: bmRequestType((uint8_t(recipient) << RecipientBits)
				| (uint8_t(type) << TypeBits)
				| (uint8_t(data_direction) << DirectionBit)),
		bRequest(breq), wValue(wval), wIndex(wid), wLength(wlen) { }

	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};
static_assert(sizeof(SetupPacket) == 8, "Bad SetupPacket size");

struct TransferStatus {
	enum {
		kActiveBit = 23,
		kStalledBit = 22,
		kDataBufferErrorBit = 21,
		kBabbleDetectedBit = 20,
		kNakReceivedBit = 19,
		kTimeOutErrorBit = 18,
		kBitstuffErrorBit = 17
	};

	static constexpr uint32_t ActLenBits = 0;
	static constexpr uint32_t StatusBits = 16;
	static constexpr uint32_t InterruptOnCompleteBits = 24;
	static constexpr uint32_t IsochronSelectBits = 25;
	static constexpr uint32_t LowSpeedBits = 26;
	static constexpr uint32_t NumErrorsBits = 27;
	static constexpr uint32_t ShortPacketDetectBits = 29;

	TransferStatus(bool ioc, bool isochron, bool spd)
	: _bits((uint32_t(1) << kActiveBit) 
			| (uint32_t(ioc) << InterruptOnCompleteBits) 
			| (uint32_t(isochron) << IsochronSelectBits) 
			| (uint32_t(spd) << ShortPacketDetectBits)) {
	
	}
	
	bool isActive() { return _bits & (1 << kActiveBit); }
	bool isStalled() { return _bits & (1 << kStalledBit); }
	bool isDataBufferError() { return _bits & (1 << kDataBufferErrorBit); }
	bool isBabbleDetected() { return _bits & (1 << kBabbleDetectedBit); }
	bool isNakReceived() { return _bits & (1 << kNakReceivedBit); }
	bool isTimeOutError() { return _bits & (1 << kTimeOutErrorBit); }
	bool isBitstuffError() { return _bits & (1 << kBitstuffErrorBit); }

	uint32_t _bits;
};

struct TransferToken {
	enum PacketId {
		kPacketIn = 0x69,
		kPacketOut = 0xE1,
		kPacketSetup = 0x2D
	};

	enum DataToggle {
		kData0 = 0,
		kData1 = 1
	};

	static constexpr uint32_t PidBits = 0;
	static constexpr uint32_t DeviceAddressBits = 8;
	static constexpr uint32_t EndpointBits = 15;
	static constexpr uint32_t DataToggleBit = 19;
	static constexpr uint32_t MaxLenBits = 21;
	
	TransferToken(PacketId packet_id, DataToggle data_toggle,
			uint8_t device_address, uint8_t endpoint_address, uint16_t max_length) 
	: _bits((uint32_t(packet_id) << PidBits)
			| (uint32_t(device_address) << DeviceAddressBits)
			| (uint32_t(endpoint_address) << EndpointBits)
			| (uint32_t(data_toggle) << DataToggleBit)
			| (uint32_t((max_length ? max_length - 1 : 0x7FF)) << MaxLenBits)) {
		assert(device_address < 128);
		assert(max_length < 2048);
	}

	uint32_t _bits;
};

struct TransferBufferPointer {
	static TransferBufferPointer from(void *item) {
		uintptr_t physical;
		HEL_CHECK(helPointerPhysical(item, &physical));
		assert((physical & 0xFFFFFFFF) == physical);
		return TransferBufferPointer(physical);
	}

	TransferBufferPointer(uint32_t pointer)
	: _bits(pointer) { }

private:
	uint32_t _bits;
};

// UHCI mandates 16 byte alignment. we align at 32 bytes
// to make sure that the TransferDescriptor does not cross a page boundary.
struct alignas(32) TransferDescriptor {
	struct LinkPointer {
		static constexpr uint32_t TerminateBit = 0;
		static constexpr uint32_t QhSelectBit = 1;
		static constexpr uint32_t VfSelectBit = 2;
		static constexpr uint32_t PointerMask = 0xFFFFFFF0;

		LinkPointer()
		: _bits(1 << TerminateBit) { }

		LinkPointer(uint32_t pointer, bool is_vf, bool is_queue)
		: _bits(pointer
				| (is_vf << VfSelectBit)
				| (is_queue << QhSelectBit)) {
			assert(pointer % 16 == 0);
		}

		bool isVf() { return _bits & (1 << VfSelectBit); }
		bool isQueue() { return _bits & (1 << QhSelectBit); }
		bool isTerminate() { return _bits & (1 << TerminateBit); }
		uint32_t actualPointer() { return _bits & PointerMask; }

		uint32_t _bits;
	};

	TransferDescriptor(TransferStatus control_status,
			TransferToken token, TransferBufferPointer buffer_pointer)
	: _controlStatus(control_status),
			_token(token), _bufferPointer(buffer_pointer) { }

	void link(TransferDescriptor *other) {
		assert(!"Implement this");
	}

	LinkPointer _linkPointer;
	TransferStatus _controlStatus;
	TransferToken _token;
	TransferBufferPointer _bufferPointer;
};

struct alignas(16) QueueHead {
	struct Pointer {
		static Pointer from(TransferDescriptor *item) {
			uintptr_t physical;
			HEL_CHECK(helPointerPhysical(item, &physical));
			assert(physical % sizeof(*item) == 0);
			assert((physical & 0xFFFFFFFF) == physical);
			return Pointer(physical, false);
		}

		static constexpr uint32_t TerminateBit = 0;
		static constexpr uint32_t QhSelectBit = 1;
		static constexpr uint32_t PointerMask = 0xFFFFFFF0;

		Pointer()
		: _bits(1 << TerminateBit) { }

		Pointer(uint32_t pointer, bool is_queue)
		: _bits(pointer
				| (is_queue << QhSelectBit)) {
			assert(pointer % 16 == 0);
		}

		bool isQueue() { return _bits & (1 << QhSelectBit);	}
		bool isTerminate() { return _bits & (1 << TerminateBit); }
		uint32_t actualPointer() { return _bits & PointerMask; }

		uint32_t _bits;
	};

	typedef Pointer LinkPointer;
	typedef Pointer ElementPointer;
	
	LinkPointer _linkPointer;
	ElementPointer _elementPointer;
};

struct FrameListPointer {
	static constexpr uint32_t TerminateBit = 0;
	static constexpr uint32_t QhSelectBit = 1;
	static constexpr uint32_t PointerMask = 0xFFFFFFF0;

	static FrameListPointer from(QueueHead *item) {
		uintptr_t physical;
		HEL_CHECK(helPointerPhysical(item, &physical));
		assert(physical % sizeof(*item) == 0);
		assert((physical & 0xFFFFFFFF) == physical);
		return FrameListPointer(physical, true);
	}

	FrameListPointer(uint32_t pointer, bool is_queue)
	: _bits(pointer
			| (is_queue << QhSelectBit)) {
		assert(pointer % 16 == 0);
	}

	bool isQueue() { return _bits & (1 << QhSelectBit); }
	bool isTerminate() { return _bits & (1 << TerminateBit); }
	uint32_t actualPointer() { return _bits & PointerMask; }

	uint32_t _bits;
};

struct FrameList {
	FrameListPointer entries[1024];
};

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects);
	void queriredDevice(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate({ "pci-vendor:0x8086", "pci-device:0x7020" },
			CALLBACK_MEMBER(this, &InitClosure::enumeratedDevice));
}

void InitClosure::enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriredDevice));
}

void InitClosure::queriredDevice(HelHandle handle) {
	enum RegisterOffset {
		kRegCommand = 0x00,
		kRegStatus = 0x02,
		kRegInterruptEnable = 0x04,
		kRegFrameNumber = 0x06,
		kRegFrameListBaseAddr = 0x08,
		kRegStartFrameModify = 0x0C,
		kRegPort1StatusControl = 0x10,
		kRegPort2StatusControl = 0x12
	};

	helx::Pipe device_pipe(handle);

	// acquire the device's resources
	printf("acquire the device's resources\n");
	HelError acquire_error;
	uint8_t acquire_buffer[128];
	size_t acquire_length;
	device_pipe.recvStringRespSync(acquire_buffer, 128, eventHub, 1, 0,
			acquire_error, acquire_length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice acquire_response;
	acquire_response.ParseFromArray(acquire_buffer, acquire_length);

	HelError bar_error;
	HelHandle bar_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 5, bar_error, bar_handle);
	HEL_CHECK(bar_error);

	assert(acquire_response.bars(4).io_type() == managarm::hw::IoType::PORT);
	HEL_CHECK(helEnableIo(bar_handle));

	HelError irq_error;
	HelHandle irq_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 7, irq_error, irq_handle);
	HEL_CHECK(irq_error);
	
	uint16_t base = acquire_response.bars(4).address();

	enum {
		kStatusInterrupt = 0x01,
		kStatusError = 0x02
	};

	auto initial_status = frigg::readIo<uint16_t>(base + kRegStatus);
	assert(!(initial_status & kStatusInterrupt));
	assert(!(initial_status & kStatusError));

	enum {
		kRootConnected = 0x0001,
		kRootConnectChange = 0x0002,
		kRootEnabled = 0x0004,
		kRootEnableChange = 0x0008,
		kRootReset = 0x0200
	};
	
	// global reset, then deassert reset and stop running the frame list
	frigg::writeIo<uint16_t>(base + kRegCommand, 0x04);
	frigg::writeIo<uint16_t>(base + kRegCommand, 0);

	// disable both ports and clear their connected/enabled changed bits
	frigg::writeIo<uint16_t>(base + kRegPort1StatusControl,
			kRootConnectChange | kRootEnableChange);
	frigg::writeIo<uint16_t>(base + kRegPort2StatusControl,
			kRootConnectChange | kRootEnableChange);

	// enable the first port and wait until it is available
	frigg::writeIo<uint16_t>(base + kRegPort1StatusControl, kRootEnabled);
	while(true) {
		auto port_status = frigg::readIo<uint16_t>(base + kRegPort1StatusControl);
		if((port_status & kRootEnabled))
			break;
	}

	// reset the first port
	frigg::writeIo<uint16_t>(base + kRegPort1StatusControl, kRootEnabled | kRootReset);
	frigg::writeIo<uint16_t>(base + kRegPort1StatusControl, kRootEnabled);
	
	auto postenable_status = frigg::readIo<uint16_t>(base + kRegStatus);
	assert(!(postenable_status & kStatusInterrupt));
	assert(!(postenable_status & kStatusError));

	// create a setup packet
	SetupPacket setup_packet(SetupPacket::kDirToDevice, SetupPacket::kDestDevice,
			SetupPacket::kStandard, 0x05, 1, 0, 0);

	TransferDescriptor transfer1(TransferStatus(true, false, false),
			TransferToken(TransferToken::kPacketSetup, TransferToken::kData0,
					0, 0, sizeof(SetupPacket)),
			TransferBufferPointer::from(&setup_packet));

	// create a queue head
	QueueHead queue_head;
	queue_head._elementPointer = QueueHead::ElementPointer::from(&transfer1);

	// setup the frame list
	HelHandle list_handle;
	HEL_CHECK(helAllocateMemory(4096, 0, &list_handle));
	void *list_mapping;
	HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
			nullptr, 0, 4096, kHelMapReadWrite, &list_mapping));
	
	auto list_pointer = (FrameList *)list_mapping;
	for(int i = 0; i < 1024; i++)
		list_pointer->entries[i] = FrameListPointer::from(&queue_head);
		
	// pass the frame list to the controller and run it
	uintptr_t list_physical;
	HEL_CHECK(helPointerPhysical(list_pointer, &list_physical));
	assert((list_physical % 0x1000) == 0);
	frigg::writeIo<uint32_t>(base + kRegFrameListBaseAddr, list_physical);
	
	auto prerun_status = frigg::readIo<uint16_t>(base + kRegStatus);
	assert(!(prerun_status & kStatusInterrupt));
	assert(!(prerun_status & kStatusError));
	
	uint16_t command_bits = 0x1;
	frigg::writeIo<uint16_t>(base + kRegCommand, command_bits);

	while(true) {
		printf("----------------------------------------\n");
		auto status = frigg::readIo<uint16_t>(base + kRegStatus);
		printf("usb status register: %d \n", status);
/*		auto port_status = frigg::readIo<uint16_t>(base + 0x10);
		printf("port status/control register:\n");
		printf("    current connect status: %d\n", port_status & (1 << 0));
		printf("    connect status change: %d\n", port_status & (1 << 1));
		printf("    port enabled: %d\n", port_status & (1 << 2));
		printf("    port enable change: %d\n", port_status & (1 << 3));
		printf("    line status: %d\n", port_status & 0x30);
		printf("    resume detect: %d\n", port_status & (1 << 6));
		printf("    always 1: %d\n", port_status & (1 << 7));
		printf("    low speed device: %d\n", port_status & (1 << 8));
		printf("    port reset: %d\n", port_status & (1 << 9));
		printf("    suspend: %d\n", port_status & (1 << 12));*/
		printf("transfer descriptor 1 status:\n");
		printf("    active: %d\n", transfer1._controlStatus.isActive());
		printf("    stalled: %d\n", transfer1._controlStatus.isStalled());
		printf("    data buffer error: %d\n", transfer1._controlStatus.isDataBufferError());
		printf("    babble detected: %d\n", transfer1._controlStatus.isBabbleDetected());
		printf("    nak received: %d\n", transfer1._controlStatus.isNakReceived());
		printf("    time out error: %d\n", transfer1._controlStatus.isTimeOutError());
		printf("    bitstuff error: %d\n", transfer1._controlStatus.isBitstuffError());
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting uhci (usb-)driver\n");

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();
	
	return 0;
}

