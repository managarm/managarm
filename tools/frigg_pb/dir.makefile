
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_OBJECTS = main.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c

.PHONY: all-$c clean-$c

all-$c: $($c_BINDIR)/frigg_pb

clean-$c:
	rm -f $($d_BINDIR)/frigg_pb $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

$($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = g++

$c_CXXFLAGS := $(CXXFLAGS)
$c_CXXFLAGS += -std=c++11

$c_LIBS = -lprotobuf -lprotoc

$($c_BINDIR)/frigg_pb: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

-include $($c_OBJECT_PATHS:%.o=%.d)

