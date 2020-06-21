#pragma once

#include <stddef.h>

namespace thor {

inline constexpr unsigned int entropySrcIrqs = 1;

void initializeRandom();

void injectEntropy(unsigned int entropySource, unsigned int seqNum, void *buffer, size_t size);

size_t generateRandomBytes(void *buffer, size_t size);

} // namespace thor
