
#include "../include/types.hpp"
#include "../include/traits.hpp"
#include "../include/initializer.hpp"

void *operator new (size_t size, void *pointer) {
	return pointer;
}

void *operator new[] (size_t size, void *pointer) {
	return pointer;
}

