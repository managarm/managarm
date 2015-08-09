
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>

void *operator new (size_t size, void *pointer) {
	return pointer;
}

void *operator new[] (size_t size, void *pointer) {
	return pointer;
}

