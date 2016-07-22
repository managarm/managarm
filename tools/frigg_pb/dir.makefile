
$(call standard_dirs)

$c_OBJECTS = main.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/frigg_pb

$c_CXX = $(HOST_CXX)

$c_CXXFLAGS := $(HOST_CPPFLAGS) $(CXXFLAGS)
$c_CXXFLAGS += -std=c++11

$c_LDFLAGS := $(HOST_LDFLAGS)

$c_LIBS = -lprotobuf -lprotoc

$($c_BINDIR)/frigg_pb: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $(HOST_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

