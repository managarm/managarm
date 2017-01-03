
$(call standard_dirs)

$c_OBJECTS := main.o frigg-glue-hel.o linker.o runtime.o \
	frigg-debug.o frigg-libc.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/ld-init.so

.PHONY: install-$c
install-$c: c := $c
install-$c: $($c_BINDIR)
	install $($c_BINDIR)/ld-init.so $(SYSROOT_PATH)/usr/lib/

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/hel/include
$c_INCLUDES += -I$(TREE_PATH)/frigg/include -I$($c_GENDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -O2 -ffreestanding -fno-exceptions -fno-rtti
$c_CXXFLAGS += -fpic -fvisibility=hidden
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_AS := x86_64-managarm-as

$c_LD := x86_64-managarm-ld
$c_LDFLAGS := -shared

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_BINDIR)/ld-init.so: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_LD) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.s | $($c_SRCDIR)
	$($d_AS) -o $@ $<

# build protobuf files
gen-$c: $($c_GENDIR)/posix.frigg_pb.hpp $($c_GENDIR)/fs.frigg_pb.hpp

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<

