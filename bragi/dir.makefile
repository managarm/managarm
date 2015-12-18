
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_GENDIR := $(BUILD_PATH)/$c/gen
$c_HEADERDIR := $(TREE_PATH)/$c/include
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_HEADERS := bragi/mbus.hpp

$c_OBJECTS := mbus.o mbus.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_TARGETS := all-$c clean-$c install-$c $($c_BINDIR)/libbragi_mbus.so $($c_BINDIR)

.PHONY: all-$c clean-$c gen-$c install-$c

all-$c: $($c_BINDIR)/libbragi_mbus.so

clean-$c:
	rm -f $($d_BINDIR)/libbragi_mbus.so $($d_OBJECT_PATHS) $($d_OBJECT_PATHS:%.o=%.d)

gen: gen-$c

install-$c:
	mkdir -p  $(SYSROOT_PATH)/usr/include/bragi
	for f in $($d_HEADERS); do \
		install $($d_HEADERDIR)/$$f $(SYSROOT_PATH)/usr/include/$$f; done
	install $($d_BINDIR)/libbragi_mbus.so $(SYSROOT_PATH)/usr/lib

$($c_GENDIR) $($c_OBJDIR) $($c_BINDIR):
	mkdir -p $@

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR) -I$($c_GENDIR) -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -fpic
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$($c_BINDIR)/libbragi_mbus.so: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -shared -o $@ $($d_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile protobuf files
gen-$c: $($c_GENDIR)/mbus.pb.tag

$c_TARGETS += $($c_GENDIR)/%.pb.tag
$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

-include $($c_OBJECT_PATHS:%.o=%.d)

