#pragma once
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

void unpoisonKasanShadow(void *pointer, size_t size);
void poisonKasanShadow(void *pointer, size_t size);
void cleanKasanShadow(void *pointer, size_t size);

void validateKasanClean(void *pointer, size_t size);

void scrubStackFrom(uintptr_t top, Continuation cont);

} // namespace thor
