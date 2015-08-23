
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_OBJECTS := main.o char_device.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c $($c_BINDIR)/vga_terminal $($c_BINDIR)

.PHONY: all-$c clean-$c

all-$c: $($c_BINDIR)/vga_terminal

clean-$c:
	rm -f $($d_BINDIR)/vga_terminal $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

$($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/bragi/include -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall

$c_LIBS := -lprotobuf-lite

$($c_BINDIR)/vga_terminal: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

-include $($c_OBJECT_PATHS:%.o=%.d)

