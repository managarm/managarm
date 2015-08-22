
namespace thor {

void setupIdt(uint32_t *table);

bool intsAreEnabled();

void enableInts();

void disableInts();

inline void halt() {
	asm volatile ( "hlt" );
}

extern "C" bool saveThisThread() __attribute__ (( returns_twice ));
extern "C" void restoreThisThread() __attribute__ (( noreturn ));

} // namespace thor


