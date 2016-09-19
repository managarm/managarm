
$(call standard_dirs)

$c_HEADERS := hel.h hel-syscalls.h helx.hpp helix/ipc.hpp helix/await.hpp

install-$c: c := $c
install-$c:
	mkdir -p $(SYSROOT_PATH)/usr/include/helix
	for f in $($c_HEADERS); do install $($c_HEADERDIR)/$$f $(SYSROOT_PATH)/usr/include/$$f; done
