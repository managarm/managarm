#ifndef THOR_GENERIC_KERNEL_HPP
#define THOR_GENERIC_KERNEL_HPP

#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/libc.hpp>
#include <frigg/initializer.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/string.hpp>
#include <frigg/array.hpp>
#include <frigg/vector.hpp>
#include <frigg/linked.hpp>
#include <frigg/priority_queue.hpp>
#include <frigg/variant.hpp>
#include <frigg/smart_ptr.hpp>

#include <frigg/arch_x86/machine.hpp>

#include "../../hel/include/hel.h"

#include "physical.hpp"
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/system.hpp>

#include "kernel_heap.hpp"
#include "schedule.hpp"
#include "core.hpp"

#include <thor.h>

namespace thor {
namespace k_init {

void main();

} } // namespace thor::k_init

#endif // THOR_GENERIC_KERNEL_HPP
