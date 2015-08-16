
$c_HEADERDIR := $(TREE_PATH)/$c/include

$c_HEADERS := hel.h hel-syscalls.h helx.hpp
$c_HEADER_PATHS := $(addprefix $($c_HEADERDIR)/,$($c_HEADERS))

$c_TARGETS := install-$c

.PHONY: all-$c clean-$c install-$c

all-$c:

clean-$c:

install-$c:
	install $($d_HEADER_PATHS) $(SYSROOT_PATH)/usr/include
