
enum DataDirection {
	kDirToDevice = 0,
	kDirToHost = 1
};

enum ControlRecipient {
	kDestDevice = 0,
	kDestInterface = 1,
	kDestEndpoint = 2,
	kDestOther = 3
};

enum ControlType {
	kStandard = 0,
	kClass = 1,
	kVendor = 2,
	kReserved = 3
};

// Alignment makes sure that a packet doesnt cross a page boundary
struct alignas(8) SetupPacket {
	enum Request {
		kGetStatus = 0x00,
		kClearFeature = 0x01,
		kSetFeature = 0x03,
		kSetAddress = 0x05,
		kGetDescriptor = 0x06,
		kSetDescriptor = 0x07,
		kGetConfig = 0x08,
		kSetConfig = 0x09
	};

	enum DescriptorType {
		kDescDevice = 0x0100,
		kDescConfig = 0x0200,
		kDescString = 0x0300,
		kDescInterface = 0x0400,
		kDescEndpoint = 0x0500
	};

	static constexpr uint8_t RecipientBits = 0;
	static constexpr uint8_t TypeBits = 5;
	static constexpr uint8_t DirectionBit = 7;

	SetupPacket(DataDirection data_direction, ControlRecipient recipient, ControlType type,
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

struct DeviceDescriptor {
	uint8_t length;
	uint8_t descriptorType;
	uint16_t bcdUsb;
	uint8_t deviceClass;
	uint8_t deviceSubclass;
	uint8_t deviceProtocol;
	uint8_t maxPacketSize;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t manufacturer;
	uint8_t product;
	uint8_t serialNumber;
	uint8_t numConfigs;
};
//FIXME: remove alignas
//static_assert(sizeof(DeviceDescriptor) == 18, "Bad DeviceDescriptor size");

struct ConfigDescriptor {
	uint8_t _length;
	uint8_t _descriptorType;
	uint16_t _totalLength;
	uint8_t _numInterfaces;
	uint8_t _configValue;
	uint8_t _iConfig;
	uint8_t _bmAttributes;
	uint8_t _maxPower;
};

