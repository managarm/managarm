
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := -iquote$($c_GENDIR) \
	$(shell $($c_PKGCONF) --cflags protobuf-lite)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -O3

$c_block_LIBS := -lhelix -lcofiber -lhw_protocol -lmbus_protocol -lblockfs \
	$(shell $($c_PKGCONF) --libs protobuf-lite)

$c_net_LIBS := -lnet \
	$(shell $($c_PKGCONF) --libs protobuf-lite)

$(call make_exec,virtio-block,main-block.o block.o virtio.o,block_)
#$(call make_exec,virtio-net,main-net.o net.o virtio.o,net_)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

