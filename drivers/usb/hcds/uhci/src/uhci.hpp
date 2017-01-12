
#include <arch/variable.hpp>

struct TransferDescriptor;
struct QueueHead;

namespace td_status {
	static constexpr arch::field<uint32_t, uint16_t> actualLength(0, 11);
	static constexpr arch::field<uint32_t, uint8_t> errorBits(17, 6);
	static constexpr arch::field<uint32_t, bool> bitstuffError(17, 1);
	static constexpr arch::field<uint32_t, bool> timeoutError(18, 1);
	static constexpr arch::field<uint32_t, bool> nakError(19, 1);
	static constexpr arch::field<uint32_t, bool> babbleError(20, 1);
	static constexpr arch::field<uint32_t, bool> bufferError(21, 1);
	static constexpr arch::field<uint32_t, bool> stalled(22, 1);
	static constexpr arch::field<uint32_t, bool> active(23, 1);
	static constexpr arch::field<uint32_t, bool> completionIrq(24, 1);
	static constexpr arch::field<uint32_t, bool> isochronous(25, 1);
	static constexpr arch::field<uint32_t, bool> lowSpeed(26, 1);
	static constexpr arch::field<uint32_t, uint8_t> numRetries(27, 2);
	static constexpr arch::field<uint32_t, bool> detectShort(28, 1);
}

struct Pointer {
	static Pointer from(TransferDescriptor *item);
	static Pointer from(QueueHead *item);

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

	TransferBufferPointer() {
		_bits = 0;
	}

	TransferBufferPointer(uint32_t pointer)
	: _bits(pointer) { }

private:
	uint32_t _bits;
};

// UHCI specifies TDs to be 32 bytes with the last 16 bytes reserved
// for the driver. We just use a 16 byte structure.
struct alignas(16) TransferDescriptor {
	typedef Pointer LinkPointer;

	TransferDescriptor(arch::bit_value<uint32_t> status,
			TransferToken token, TransferBufferPointer buffer_pointer)
	: status{status}, _token(token), _bufferPointer(buffer_pointer) { }

	void dumpStatus() {
		if(status.load() & td_status::active) printf(" active");
		if(status.load() & td_status::stalled) printf(" stalled");
		if(status.load() & td_status::bitstuffError) printf(" bitstuff-error");
		if(status.load() & td_status::timeoutError) printf(" time-out");
		if(status.load() & td_status::nakError) printf(" nak");
		if(status.load() & td_status::babbleError) printf(" babble-detected");
		if(status.load() & td_status::bufferError) printf(" data-buffer-error");
	}

	LinkPointer _linkPointer;
	arch::bit_variable<uint32_t> status;
	TransferToken _token;
	TransferBufferPointer _bufferPointer;
};

static_assert(sizeof(TransferDescriptor) == 16, "Bad sizeof(TransferDescriptor)");

struct alignas(16) QueueHead {
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

enum {
	kPciLegacySupport = 0xC0
};

enum RegisterOffset {
	kRegCommand = 0x00,
	kRegStatus = 0x02,
	kRegInterruptEnable = 0x04,
	kRegFrameNumber = 0x06,
	kRegFrameListBaseAddr = 0x08,
	kRegStartFrameModify = 0x0C,
	kRegPort1StatusControl = 0x10,
	kRegPort2StatusControl = 0x12,
};

enum {
	kStatusInterrupt = 0x01,
	kStatusError = 0x02
};

enum {
	kRootConnected = 0x0001,
	kRootConnectChange = 0x0002,
	kRootEnabled = 0x0004,
	kRootEnableChange = 0x0008,
	kRootReset = 0x0200
};
	
