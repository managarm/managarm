
$(call standard_dirs)

$c_HEADERS := hel.h hel-syscalls.h helx.hpp
$c_HEADER_PATHS := $(addprefix $($c_HEADERDIR)/,$($c_HEADERS))

install-$c:
	install $($d_HEADER_PATHS) $(SYSROOT_PATH)/usr/include
