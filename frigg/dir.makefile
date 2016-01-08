
$(call standard_dirs)

$c_HEADERS := frigg/c-support.h frigg/cxx-support.hpp frigg/traits.hpp \
		frigg/debug.hpp frigg/algorithm.hpp frigg/callback.hpp frigg/optional.hpp \
		frigg/memory.hpp frigg/memory-slab.hpp \
		frigg/string.hpp frigg/vector.hpp frigg/protobuf.hpp frigg/atomic.hpp \
		frigg/arch_x86/atomic_impl.hpp

install-$c:
	mkdir -p  $(SYSROOT_PATH)/usr/include/frigg
	mkdir -p  $(SYSROOT_PATH)/usr/include/frigg/arch_x86
	for f in $($d_HEADERS); do \
		install $($d_HEADERDIR)/$$f $(SYSROOT_PATH)/usr/include/$$f; done

