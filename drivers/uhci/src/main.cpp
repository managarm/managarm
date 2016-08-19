
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

struct SetupPacket {
	struct RequestType {
		enum DataDirection {
			kHostToDevice = 0,
			kDeviceToHost = 1
		};

		enum Type {
			kStandard = 0,
			kClass = 1,
			kVendor = 2,
			kReserved = 3
		};

		enum Recipient {
			kDevice = 0,
			kInterface = 1,
			kEndpoint = 2,
			kOhter = 3
		};

		static constexpr uint8_t RecipientOffset = 0;
		static constexpr uint8_t TypeOffset = 5;
		static constexpr uint8_t DataDirectionOffset = 7;

		RequestType(uint8_t recipient, uint8_t type, uint8_t data_direction)
		: _request(recipient << RecipientOffset | type << TypeOffset |
				data_direction << DataDirectionOffset) {
			
		}

		uint8_t _request;
	};

	SetupPacket(RequestType req_type, uint8_t breq, uint16_t wval, uint16_t wid, uint16_t wlen)
	: bmRequestType(req_type), bRequest(breq), wValue(wval), wIndex(wid), wLength(wlen) {

	};

	RequestType bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

struct FrameListPointer {
	static constexpr uint32_t TerminateBit = 0;
	static constexpr uint32_t QhSelectBit = 1;
	static constexpr uint32_t PointerMask = 0xFFFFFFF0;

	FrameListPointer(uint32_t pointer, bool is_queue, bool is_terminate)
	: _bits(pointer | (is_queue << QhSelectBit) | (is_terminate << TerminateBit)) {
		assert(pointer % 16 == 0);
	}

	bool isQueue() {
		return _bits & (1 << QhSelectBit);
	}

	bool isTerminate() {
		return _bits & (1 << TerminateBit);
	}

	uint32_t actualPointer() {
		return _bits & PointerMask;
	}

	uint32_t _bits;
};

struct FrameList {
	FrameListPointer entries[1024];
};

struct alignas(16) TransferDescriptor {
	struct LinkPointer {
		static constexpr uint32_t TerminateBit = 0;
		static constexpr uint32_t QhSelectBit = 1;
		static constexpr uint32_t VfSelectBit = 2;
		static constexpr uint32_t PointerMask = 0xFFFFFFF0;

		LinkPointer(uint32_t pointer, bool is_vf, bool is_queue, bool is_terminate) 
		: _bits(pointer | (is_vf << VfSelectBit) | (is_queue << QhSelectBit) | (is_terminate << TerminateBit)){
			assert(pointer % 16 == 0);
		}

		bool isVf() {
			return _bits & (1 << VfSelectBit);
		}

		bool isQueue() {
			return _bits & (1 << QhSelectBit);
		}

		bool isTerminate() {
			return _bits & (1 << TerminateBit);
		}

		uint32_t actualPointer() {
			return _bits & PointerMask;
		}

		uint32_t _bits;
	};

	struct ControlStatus {
		ControlStatus() {
			_bits = 0;
		}

		uint32_t _bits;
	};

	struct Token {
		enum PacketId {
			kPacketIn = 0x69,
			kPacketOut = 0xE1,
			kPacketSetup = 0x2D
		};

		enum DataPid {
			DATA0 = 0,
			DATA1 = 1
		};

		static constexpr uint32_t PidBits = 0;
		static constexpr uint32_t DeviceAddressBits = 8;
		static constexpr uint32_t EndpointBits = 15;
		static constexpr uint32_t DataToggleBit = 19;
		static constexpr uint32_t MaxLenBits = 21;
		
		Token(PacketId packet_id, uint8_t device_address, uint8_t endpoint_address,
				DataPid data_pid, uint16_t max_len) 
		: _bits(packet_id << PidBits | device_address << DeviceAddressBits |
				endpoint_address << EndpointBits | data_pid << DataToggleBit |
				max_len << MaxLenBits) {
			assert(device_address < 128);
			assert(max_len < 2048);
		}

		uint32_t _bits;
	};

	TransferDescriptor(LinkPointer link_pointer, ControlStatus control_status,
			Token token, uint32_t buffer_pointer)
	: _linkPointer(link_pointer), _controlStatus(control_status),
			_token(token), _bufferPointer(buffer_pointer) {
	
	}

	LinkPointer _linkPointer;
	ControlStatus _controlStatus;
	Token _token;
	uint32_t _bufferPointer;
};

struct alignas(16) QueueHead {
	struct Pointer {
		static constexpr uint32_t TerminateBit = 0;
		static constexpr uint32_t QhSelectBit = 1;
		static constexpr uint32_t PointerMask = 0xFFFFFFF0;

		Pointer(uint32_t pointer, bool is_queue, bool is_terminate)
		: _bits(pointer | (is_queue << QhSelectBit) | (is_terminate << TerminateBit)) {
			assert(pointer % 16 == 0);
		}

		bool isQueue() {
			return _bits & (1 << QhSelectBit);
		}

		bool isTerminate() {
			return _bits & (1 << TerminateBit);
		}

		uint32_t actualPointer() {
			return _bits & PointerMask;
		}

		uint32_t _bits;
	};

	typedef Pointer LinkPointer;
	typedef Pointer ElementPointer;

	QueueHead(LinkPointer link_pointer, ElementPointer element_pointer)
	: _linkPointer(link_pointer), _elementPointer(element_pointer) {

	}
	
	LinkPointer _linkPointer;
	ElementPointer _elementPointer;
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

	assert(acquire_response.bars(0).io_type() == managarm::hw::IoType::PORT);
	HEL_CHECK(helEnableIo(bar_handle));

	HelError irq_error;
	HelHandle irq_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 7, irq_error, irq_handle);
	HEL_CHECK(irq_error);



	HelHandle list_handle;
	HEL_CHECK(helAllocateMemory(4096, 0, &list_handle));
	void *list_mapping;
	HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
			nullptr, 0, 4096, kHelMapReadWrite, &list_mapping));

	auto list_pointer = (FrameList *)list_mapping;

	
	QueueHead::LinkPointer link_pointer(0, 1, 0);
	QueueHead::ElementPointer element_pointer(0, 1, 0);

	QueueHead queue_head(link_pointer, element_pointer);
	uintptr_t queue_physical;
	HEL_CHECK(helPointerPhysical(&queue_head, &queue_physical));
	
	for(int i = 0; i < 1024; i++) {
		list_pointer->entries[i] = FrameListPointer(queue_physical, true, false);
	}

	uintptr_t list_physical;
	HEL_CHECK(helPointerPhysical(list_pointer, &list_physical));

	uint16_t base = acquire_response.bars(4).address();
	frigg::writeIo<uint32_t>(base + kRegFrameListBaseAddr, list_physical);

	uint16_t command_bits = 0x1;
	frigg::writeIo<uint16_t>(base + kRegCommand, command_bits);

	while(true) {
		auto status = frigg::readIo<uint16_t>(base + kRegStatus);
		printf("status: %d \n", status);
	}

	
	SetupPacket::RequestType req_type(
			SetupPacket::RequestType::Recipient::kDevice,
			SetupPacket::RequestType::Type::kStandard,
			SetupPacket::RequestType::DataDirection::kDeviceToHost);
	SetupPacket setup_packet(req_type, 6, 0x0100, 0, 18);
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

