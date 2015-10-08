
#include <frigg/smart_ptr.hpp>
#include <frigg/glue-hel.hpp>
#include <frigg/algorithm.hpp>
#include <helx.hpp>

template<typename T>
using StdSharedPtr = frigg::SharedPtr<T>;

template<typename T>
using StdUnsafePtr = frigg::UnsafePtr<T>;

extern helx::EventHub eventHub;
extern helx::Client mbusConnect;
extern helx::Client ldServerConnect;
extern helx::Pipe ldServerPipe;

