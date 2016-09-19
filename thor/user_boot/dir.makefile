
$(call standard_dirs)

$c_OBJECTS := main.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/user_boot

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include
$c_INCLUDES += -I$($c_GENDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LDFLAGS := -l:ld-init.so
$c_LIBS := -lcofiber

$($c_BINDIR)/user_boot: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# generate protobuf
gen-$c:  $($c_GENDIR)/xuniverse.pb.tag $($c_GENDIR)/posix.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

