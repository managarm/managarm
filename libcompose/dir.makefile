
$(call standard_dirs)

$c_HEADERS := libcompose.hpp

$c_OBJECTS := compose.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/libcompose.so

install-$c:
	mkdir -p  $(SYSROOT_PATH)/usr/include/bragi
	for f in $($d_HEADERS); do \
		install $($d_HEADERDIR)/$$f $(SYSROOT_PATH)/usr/include/$$f; done
	install $($d_BINDIR)/libcompose.so $(SYSROOT_PATH)/usr/lib

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR) -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -fpic
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$($c_BINDIR)/libcompose.so: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -shared -o $@ $($d_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

