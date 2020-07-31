#pragma once

namespace thor {

void unpoisonKasanShadow(void *pointer, size_t size);
void poisonKasanShadow(void *pointer, size_t size);
void cleanKasanShadow(void *pointer, size_t size);

} // namespace thor
