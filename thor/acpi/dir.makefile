
$(call standard_dirs)
$(call define_objdir,ACPICA_OBJ,$(BUILD_PATH)/$c/acpica-obj)

$c_INCLUDE := -I$($c_GENDIR)
$c_INCLUDE += -I$(TREE_PATH)/$c/acpica/source/include -I$(TREE_PATH)/frigg/include

$c_CC := x86_64-managarm-gcc
$c_CCFLAGS := $($c_INCLUDE)

$c_CXX := x86_64-managarm-g++
$c_CXXFLAGS := -std=c++14 $($c_INCLUDE)
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LDFLAGS :=
$c_LIBS := -lhelix -lprotobuf-lite -lcofiber -lmbus

$c_OBJECTS := main.o pci_io.o pci_discover.o glue-acpica.o hw.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

# configure ACPICA paths
$c_ACPICA_SRCDIR = $(TREE_PATH)/$c/acpica/source/components
$c_find_acpica = $(patsubst $($c_ACPICA_SRCDIR)/%.c,%.o,$(wildcard $($c_ACPICA_SRCDIR)/$x/*))

ACPICA_SUBDIRS := events namespace tables hardware utilities \
	executer disassembler debugger dispatcher resources parser
$c_ACPICA_SUBDIR_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$(ACPICA_SUBDIRS))

$c_ACPICA_OBJECTS := $(foreach x,$(ACPICA_SUBDIRS),$($c_find_acpica))
$c_ACPICA_OBJECT_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$($c_ACPICA_OBJECTS))

$(call decl_targets,$c_ACPICA_OBJDIR/%)

all-$c: $($c_BINDIR)/acpi

$($c_ACPICA_SUBDIR_PATHS):
	mkdir -p $@

$($c_BINDIR)/acpi: $($c_OBJECT_PATHS) $($c_ACPICA_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_ACPICA_OBJECT_PATHS) $($d_LIBS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cc | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# compile ACPICA
$($c_ACPICA_OBJDIR)/%.o: $($c_ACPICA_SRCDIR)/%.c | $($c_ACPICA_SUBDIR_PATHS)
	$($d_CC) -c -o $@ $($d_CCFLAGS) $<
	$($d_CC) $($d_CCFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# generate protobuf
gen-$c: $($c_GENDIR)/hw.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<
	touch $@

