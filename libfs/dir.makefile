
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_HEADERDIR := $(TREE_PATH)/$c/include
$c_GENDIR := $(BUILD_PATH)/$c/gen
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_OBJECTS := libfs.o gpt.o ext2fs.o fs.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c install-$c $($c_BINDIR)/libfs.so $($c_BINDIR)

.PHONY: all-$c clean-$c install-$c gen-$c

all-$c: $($c_BINDIR)/libfs.so

clean-$c:
	rm -f $($d_BINDIR)/libfs.so $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

install-$c:
	install $($d_HEADERDIR)/libfs.hpp $(SYSROOT_PATH)/usr/include
	install $($d_BINDIR)/libfs.so $(SYSROOT_PATH)/usr/lib

$($c_GENDIR) $($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := -I$(TREE_PATH)/bragi/include -I$(TREE_PATH)/frigg/include \
	-I$($c_HEADERDIR) -I$($c_GENDIR) \
	$(shell $($c_PKGCONF) --cflags protobuf-lite)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -O2 -fPIC
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS := -lbragi_mbus \
	$(shell $($c_PKGCONF) --libs protobuf-lite)

$($c_BINDIR)/libfs.so: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -shared -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile protobuf files
$c_TARGETS += $($c_GENDIR)/%
gen-$c: $($c_GENDIR)/fs.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

-include $($c_OBJECT_PATHS:%.o=%.d)

