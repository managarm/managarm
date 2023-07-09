#ifndef HEL_STUBS_H
#define HEL_STUBS_H

#if defined(__x86_64__)
#	include "hel-stubs-x86_64.h"
#elif defined(__aarch64__)
#	include "hel-stubs-aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#	include "hel-stubs-riscv64.h"
#else
#	error Unsupported architecture
#endif

#endif
