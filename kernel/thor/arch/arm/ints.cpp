#include <thor-internal/arch/ints.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

void suspendSelf() { assert(!"Not implemented"); }

void sendPingIpi(int id) {
	thor::infoLogger() << "sendPingIpi is unimplemented" << frg::endlog;
}

}
