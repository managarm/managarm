
#include <frigg/smart_ptr.hpp>
#include <frigg/glue-hel.hpp>
#include <frigg/algorithm.hpp>
#include <helx.hpp>

template<typename T>
using StdSharedPtr = frigg::SharedPtr<T, Allocator>;

template<typename T>
using StdUnsafePtr = frigg::UnsafePtr<T, Allocator>;

extern helx::Client ldServerConnect;

