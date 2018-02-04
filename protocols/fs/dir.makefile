
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := $(shell $($c_PKGCONF) --cflags protobuf-lite)
$c_INCLUDES += -I$($c_HEADERDIR) -iquote$($c_GENDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++17 -Wall -Wextra -fPIC -O2

$c_LIBS := -lhelix -lcofiber \
	$(shell $($c_PKGCONF) --libs protobuf-lite)

$(call make_so,libfs_protocol.so,client.o server.o fs.pb.o)
$(call install_header,protocols/fs/client.hpp)
$(call install_header,protocols/fs/common.hpp)
$(call install_header,protocols/fs/server.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

# compile protobuf files
gen-$c: $($c_GENDIR)/fs.pb.tag

$(call gen_protobuf_cpp,$(TREE_PATH)/bragi/proto,$($c_GENDIR))
$(call compile_cxx,$($c_GENDIR),$($c_OBJDIR))
