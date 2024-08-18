#include <async/basic.hpp>
#include <cstdint>
#include <initializer_list>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/rtl8168.hpp>
#include <nic/rtl8168/regs.hpp>
#include <frg/logging.hpp>
#include <helix/timer.hpp>
#include <memory>
#include <unistd.h>

async::result<void> RealtekNic::writeRTL8168gMDIO(int reg, int val) {
	if(reg == 0x1F) { assert(!"Not implemented"); }

	// TODO: OCP base memes


}

async::result<int> RealtekNic::readRTL8168gMDIO(int reg) {
	if(reg == 0x1F) { assert(!"Not implemented"); }


	co_return 0;
}


async::result<void> RealtekNic::writePHY(int reg, int val) {
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			assert(!"Not implemented");
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer63: {
			co_await writeRTL8168gMDIO(reg, val);
			break;
		}
		default: {
			assert(!"Not implemented");
			break;
		}
	}
}

async::result<int> RealtekNic::readPHY(int reg) {
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			assert(!"Not implemented");
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer63: {
			co_return co_await readRTL8168gMDIO(reg);
		}
		default: {
			assert(!"Not implemented");
			break;
		}
	}

	assert(!"Not reachable");
	__builtin_unreachable();
}

// TODO: implement PHY control
//       this is kind of weird, but we can (hopefully...) get
//       away with not doing this for now PHY bring-up is partially
//       implemented in a off-driver system in linux, since the phy
//       models are not really network card dependent
void RealtekNic::configurePHY() {
//	switch(_revision) {
//		case MacRevision::MacVer42 ... MacRevision::MacVer44: {
//
//		}
//		default: {
//			std::cerr << "drivers/rtl8168: PHY configure for MacVer" << _revision << " is not implemented!" << std::endl;
//			assert(!"Not implemented");
//		}
//	}
}
