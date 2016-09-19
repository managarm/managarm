
$(call standard_dirs)

$c_OBJECTS := main.o frigg-glue-hel.o frigg-debug.o frigg-libc.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/mbus

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_GENDIR) -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LDFLAGS :=

$($c_BINDIR)/mbus: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# build frigg source files
$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# build protobuf files
gen-$c: $($c_GENDIR)/mbus.frigg_pb.hpp

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<

