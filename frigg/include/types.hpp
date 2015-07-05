
typedef char int8_t;
typedef unsigned char uint8_t;

#if defined(__i386__)
#include "arch_x86/types32.hpp"
#elif defined(__x86_64__)
#include "arch_x86/types64.hpp"
#endif

