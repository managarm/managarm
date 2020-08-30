
#include <frigg/arch_x86/tss.hpp>

namespace frigg {
namespace arch_x86 {

void initializeTss64(Tss64 *tss) {
	tss->ioMapOffset = __builtin_offsetof(Tss64, ioBitmap);

	for(int i = 0; i < 8192; i++)
		tss->ioBitmap[i] = 0xFF;
	tss->ioAllOnes = 0xFF;
}

} } // namespace frigg::arch_x86
 
