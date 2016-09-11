
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

	bool isAnyError() { 
		return isStalled() 
			|| isDataBufferError()
			|| isBabbleDetected() 
			|| isNakReceived() 
			|| isTimeOutError() 
			|| isBitstuffError(); 
	}


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

// UHCI mandates 16 byte alignment. we align at 32 bytes
// to make sure that the TransferDescriptor does not cross a page boundary.
struct alignas(32) TransferDescriptor {
	struct LinkPointer {
		static LinkPointer from(TransferDescriptor *item) {
			uintptr_t physical;
			HEL_CHECK(helPointerPhysical(item, &physical));
			assert(physical % sizeof(*item) == 0);
			assert((physical & 0xFFFFFFFF) == physical);
			return LinkPointer(physical, false, false);
		}

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

	void dumpStatus() {
		if(_controlStatus.isActive()) printf(" active");
		if(_controlStatus.isStalled()) printf(" stalled");
		if(_controlStatus.isDataBufferError()) printf(" data-buffer-error");
		if(_controlStatus.isBabbleDetected()) printf(" babble-detected");
		if(_controlStatus.isNakReceived()) printf(" nak");
		if(_controlStatus.isTimeOutError()) printf(" time-out");
		if(_controlStatus.isBitstuffError()) printf(" bitstuff-error");
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
		static Pointer from(QueueHead *item) {
			uintptr_t physical;
			HEL_CHECK(helPointerPhysical(item, &physical));
			assert(physical % sizeof(*item) == 0);
			assert((physical & 0xFFFFFFFF) == physical);
			return Pointer(physical, true);
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

enum {
	kStatusInterrupt = 0x01,
	kStatusError = 0x02
};

