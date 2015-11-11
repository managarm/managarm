
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_GENDIR := $(BUILD_PATH)/$c/gen
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_OBJECTS := main.o mbus.pb.o input.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c gen-$c

.PHONY: all-$c clean-$c

all-$c: $($c_BINDIR)/kbd

clean-$c:
	rm -f $($d_BINDIR)/kbd $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

gen: gen-$c

$($c_GENDIR) $($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_GENDIR) -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS := -lbragi_mbus -lprotobuf-lite

$($c_BINDIR)/kbd: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile protobuf files
gen-$c: $($c_GENDIR)/mbus.pb.tag  $($c_GENDIR)/input.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

-include $($c_OBJECT_PATHS:%.o=%.d)

