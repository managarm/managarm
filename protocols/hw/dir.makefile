
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := $(shell $($c_PKGCONF) --cflags protobuf-lite)
$c_INCLUDES += -iquote$($c_GENDIR) -I$($c_HEADERDIR)
$c_INCLUDES += -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -fPIC -O2
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS := -lhelix -lcofiber \
	$(shell $($c_PKGCONF) --libs protobuf-lite)

$(call make_so,libhw_protocol.so,client.o hw.pb.o)
$(call install_header,protocols/hw/client.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

# compile protobuf files
gen-$c: $($c_GENDIR)/hw.pb.tag

$(call gen_protobuf_cpp,$(TREE_PATH)/$c,$($c_GENDIR))
$(call compile_cxx,$($c_GENDIR),$($c_OBJDIR))
