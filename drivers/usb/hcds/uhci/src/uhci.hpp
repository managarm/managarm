
#include <arch/variable.hpp>

struct TransferDescriptor;
struct QueueHead;

enum class Packet : uint8_t {
	in = 0x69,
	out = 0xE1,
	setup = 0x2D
};

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

namespace td_token {
	static constexpr arch::field<uint32_t, Packet> pid(0, 8);
	static constexpr arch::field<uint32_t, uint8_t> address(8, 7);
	static constexpr arch::field<uint32_t, uint8_t> pipe(15, 4);
	static constexpr arch::field<uint32_t, unsigned int> toggle(19, 1);
	static constexpr arch::field<uint32_t, size_t> length(21, 11);
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
			arch::bit_value<uint32_t> token, TransferBufferPointer buffer_pointer)
	: status{status}, token{token}, _bufferPointer(buffer_pointer) { }

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
	arch::bit_variable<uint32_t> token;
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
	
