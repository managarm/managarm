
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_GENDIR := $(BUILD_PATH)/$c/gen
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_OBJECTS := main.o device.o vfs.o process.o exec.o \
		dev_fs.o pts_fs.o sysfile_fs.o extern_fs.o \
		frigg-glue-hel.o frigg-debug.o frigg-libc.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c $($c_BINDIR)/posix-subsystem $($c_BINDIR)

.PHONY: all-$c clean-$c gen-$c

all-$c: $($c_BINDIR)/posix-subsystem

clean-$c:
	rm -f $($d_BINDIR)/posix-subsystem $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

$($c_GENDIR) $($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -I$($c_GENDIR)
$c_CXXFLAGS += -std=c++1y -Wall -ffreestanding -fno-exceptions -fno-rtti
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_LDFLAGS := -nostdlib

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_BINDIR)/posix-subsystem: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# generate protobuf
$c_TARGETS += $($c_GENDIR)/%
gen-$c: $($c_GENDIR)/posix.frigg_pb.hpp $($c_GENDIR)/mbus.frigg_pb.hpp \
		$($c_GENDIR)/fs.frigg_pb.hpp $($c_GENDIR)/ld-server.frigg_pb.hpp

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<

-include $($c_OBJECT_PATHS:%.o=%.d)

