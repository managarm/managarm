
# configure the driver's paths and tools
$c_SRCDIR := $(TREE_PATH)/$c/src
$c_GENDIR := $(BUILD_PATH)/$c/gen
$c_OBJDIR := $(BUILD_PATH)/$c/obj
$c_BINDIR := $(BUILD_PATH)/$c/bin

$c_CC := x86_64-managarm-gcc
$c_INCLUDE := -I$(TREE_PATH)/$c/acpica/source/include -I$(TREE_PATH)/frigg/include
$c_CCFLAGS := $($c_INCLUDE)

$c_CXX := x86_64-managarm-g++
$c_CXXFLAGS := -std=c++11 -fno-rtti -fno-exceptions $($c_INCLUDE)

$c_LDFLAGS := -nostdlib

$c_OBJECTS := main.o glue-acpica.o frigg-glue-hel.o frigg-debug.o frigg-initializer.o frigg-libc.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

# configure ACPICA paths
$c_ACPICA_SRCDIR = $(TREE_PATH)/$c/acpica/source/components
$c_ACPICA_OBJDIR := $(BUILD_PATH)/$c/acpica-obj
$c_find_acpica = $(patsubst $($c_ACPICA_SRCDIR)/%.c,%.o,$(wildcard $($c_ACPICA_SRCDIR)/$x/*))

ACPICA_SUBDIRS := events namespace tables hardware utilities \
	executer disassembler debugger dispatcher resources parser
$c_ACPICA_SUBDIR_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$(ACPICA_SUBDIRS))

$c_ACPICA_OBJECTS := $(foreach x,$(ACPICA_SUBDIRS),$($c_find_acpica))
$c_ACPICA_OBJECT_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$($c_ACPICA_OBJECTS))

$c_TARGETS := all-$c clean-$c $($c_BINDIR)/acpi
$c_TARGETS += $($c_OBJECT_PATHS) $($c_ACPICA_OBJECT_PATHS)

.PHONY: all-$c clean-$c

all-$c: $($c_BINDIR)/acpi

clean-$c:
	rm -f $($d_OBJECT_PATHS) $($d_ACPICA_OBJECT_PATHS)

$($c_GENDIR) $($c_OBJDIR) $($c_BINDIR) $($c_ACPICA_SUBDIR_PATHS):
	mkdir -p $@

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_BINDIR)/acpi: $($c_OBJECT_PATHS) $($c_ACPICA_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_ACPICA_OBJECT_PATHS)

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_ACPICA_OBJDIR)/%.o: $($c_ACPICA_SRCDIR)/%.c | $($c_ACPICA_SUBDIR_PATHS)
	$($d_CC) -c -o $@ $($d_CCFLAGS) $<
	$($d_CC) $($d_CCFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

#-include $(OBJDIR_OBJECTS:%.o=%.d)

