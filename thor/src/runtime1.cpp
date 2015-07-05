
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"

ThorRtThreadState *thorRtUserContext = nullptr;

void *operator new (size_t size, void *pointer) {
	return pointer;
}

void __cxa_pure_virtual() {
	thor::debug::criticalLogger->log("Pure virtual call");
	thorRtHalt();
}

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax");
};

void ioWait() { }

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

enum PicRegisters : uint16_t {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum PicBytes : uint8_t {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kPicEoi = 0x20
};

void thorRtRemapPic(int offset) {
	// save masks
	uint8_t a1 = ioInByte(kPic1Data);
	uint8_t a2 = ioInByte(kPic2Data);

	// start initilization
	ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	ioOutByte(kPic1Data, offset);
	ioWait();
	ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	ioOutByte(kPic1Data, 4);
	ioWait();
	ioOutByte(kPic2Data, 2);
	ioWait();

	ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	ioOutByte(kPic1Data, a1);
	ioOutByte(kPic2Data, a2);
}

void thorRtSetupIrqs() {
	thorRtRemapPic(64);
}

void thorRtAcknowledgeIrq(int irq) {
	if(irq >= 8)
		ioOutByte(kPic2Command, kPicEoi);
	ioOutByte(kPic1Command, kPicEoi);
}

