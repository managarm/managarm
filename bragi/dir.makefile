
$(call standard_dirs)

$c_HEADERS := bragi/mbus.hpp

$c_OBJECTS := mbus.o mbus.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/libbragi_mbus.so

install-$c:
	mkdir -p  $(SYSROOT_PATH)/usr/include/bragi
	for f in $($d_HEADERS); do \
		install $($d_HEADERDIR)/$$f $(SYSROOT_PATH)/usr/include/$$f; done
	install $($d_BINDIR)/libbragi_mbus.so $(SYSROOT_PATH)/usr/lib

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := -I$($c_HEADERDIR) -I$($c_GENDIR) -I$(TREE_PATH)/frigg/include \
	$(shell $($c_PKGCONF) --cflags protobuf-lite)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -fpic
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS := $(shell $($c_PKGCONF) --libs protobuf-lite)

$($c_BINDIR)/libbragi_mbus.so: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -shared -o $@ $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile protobuf files
gen-$c: $($c_GENDIR)/mbus.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

