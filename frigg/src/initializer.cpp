
#include "../include/types.hpp"
#include "../include/utils.hpp"
#include "../include/initializer.hpp"

void *operator new (size_t size, void *pointer) {
	return pointer;
}

