
namespace op_regs {
	arch::bit_register<uint32_t> usbcmd(0);
	arch::bit_register<uint32_t> usbsts(0x04);
	arch::bit_register<uint32_t> usbintr(0x08);
	arch::scalar_register<uint32_t> asynclistaddr(0x18);
	arch::bit_register<uint32_t> hcsparams(0x04);
	arch::scalar_register<uint32_t> configflag(0x40);
}

namespace cap_regs {
	arch::scalar_register<uint8_t> caplength(0);
}

namespace port_regs {
	arch::bit_register<uint32_t> sc(0);
}

namespace usbcmd {
	arch::field<uint32_t, bool> run(0, 1);
	arch::field<uint32_t, bool> hcReset(1, 1);
	arch::field<uint32_t, bool> asyncEnable(5, 1);
	arch::field<uint32_t, uint8_t> irqThreshold(16, 8);
}

namespace usbsts {
	arch::field<uint32_t, bool> transactionIrq(0, 1);
	arch::field<uint32_t, bool> errorIrq(1, 1);
	arch::field<uint32_t, bool> portChange(2, 1);
	arch::field<uint32_t, bool> hostError(4, 1);
	arch::field<uint32_t, bool> hcHalted(12, 1);
}

namespace usbintr {
	arch::field<uint32_t, bool> transaction(0, 1);
	arch::field<uint32_t, bool> usbError(1, 1);
	arch::field<uint32_t, bool> portChange(2, 1);
	arch::field<uint32_t, bool> hostError(4, 1);
}

namespace hcsparams {
	arch::field<uint32_t, uint8_t> nPorts(0, 4);
	arch::field<uint32_t, bool> portPower(4, 1);
}

namespace portsc {
	arch::field<uint32_t, bool> connectStatus(0, 1);
	arch::field<uint32_t, bool> connectChange(1, 1);
	arch::field<uint32_t, bool> portStatus(2, 1);
	arch::field<uint32_t, bool> portChange(3, 1);
	arch::field<uint32_t, bool> portReset(8, 1);
	arch::field<uint32_t, uint8_t> lineStatus(10, 2);
	arch::field<uint32_t, bool> portOwner(13, 1);
}

struct QueueHead {
	uint32_t queueHeadHLPointer;
	uint8_t deviceAddress;
	uint8_t endPoint;
	uint16_t maxPacketLen;
	uint8_t sMask;
	uint8_t cMask;
	uint16_t hubAddr;
	uint32_t currentTd;
	uint32_t nextTd;
	uint32_t alternateNextTd;
	uint32_t totalBytes;
	uint32_t bufferPtr0;
	uint32_t bufferPtr1;
	uint32_t bufferPtr2;
	uint32_t bufferPtr3;
	uint32_t bufferPtr4;
};

