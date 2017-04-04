
#ifndef KBD_SPEC_HPP
#define KBD_SPEC_HPP

constexpr int DATA = 0x60;
constexpr int STATUS = 0x64;
constexpr int COMMAND = 0x64;

constexpr int readByte0 = 0x20;
constexpr int writeByte0 = 0x60;
constexpr int disable2ndPort = 0xA7;
constexpr int enable2ndPort = 0xA8;
constexpr int disable1stPort = 0xAD;
constexpr int enable1stPort = 0xAE;
constexpr int write2ndNextByte = 0xD4;

namespace kbd_register {
	arch::scalar_register<uint8_t> data(0);
	arch::bit_register<uint8_t> status(4);
	arch::scalar_register<uint8_t> command(4);
}

namespace status_bits {
	arch::field<uint8_t, bool> outBufferStatus(0, 1);
	arch::field<uint8_t, bool> inBufferStatus(1, 1);
	arch::field<uint8_t, bool> sysFlag(2, 1);
	arch::field<uint8_t, bool> cmdData(3, 1);
	arch::field<uint8_t, bool> timeoutError(6, 1);
	arch::field<uint8_t, bool> parityError(7, 1);
}

#endif // KBD_SPEC_HPP

