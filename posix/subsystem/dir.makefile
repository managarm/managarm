
$(call standard_dirs)
$(call define_objdir,DEVICES_OBJ,$($c_OBJDIR)/devices)

$c_OBJECTS := main.o vfs.o process.o device.o exec.o
$c_OBJECTS += devices/helout.o
$c_OBJECTS += tmp_fs.o extern_fs.o
$c_OBJECTS += posix.pb.o
# fs.pb.o is part of libfs_protocol.
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/posix-subsystem

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include -iquote$($c_SRCDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -I$($c_GENDIR)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -O2
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LDFLAGS :=
$c_LIBS := -lhelix -lprotobuf-lite -lcofiber -lmbus_protocol
$c_LIBS += -lfs_protocol

$($c_BINDIR)/posix-subsystem: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/devices/%.o: $($c_SRCDIR)/devices/%.cpp | $($c_DEVICES_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile protobuf
$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# generate protobuf
gen-$c: $($c_GENDIR)/posix.pb.tag $($c_GENDIR)/fs.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

