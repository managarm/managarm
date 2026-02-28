#include <thor-internal/pfn-db.hpp>

namespace thor {

static frg::eternal<PfnDb> pfnDbSingleton;

PfnDb &globalPfnDb() {
	return pfnDbSingleton.get();
}

} // namespace thor
