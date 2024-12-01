#include <thor-internal/cpu-data.hpp>
#include <thor-internal/traps.hpp>

namespace thor {

bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) {
	if(inHigherHalf(address))
		return false;

	auto uar = getCpuData()->currentUar;
	if(!uar)
		return false;

	auto ip = *accessor.ip();
	if(!(ip >= reinterpret_cast<uintptr_t>(uar->startIp)
			&& ip < reinterpret_cast<uintptr_t>(uar->endIp)))
		return false;

	if(write) {
		if(!(uar->flags & uarWrite))
			return false;
	}else{
		if(!(uar->flags & uarRead))
			return false;
	}

	*accessor.ip() = reinterpret_cast<Word>(uar->faultIp);
	return true;
}

} // namespace thor
