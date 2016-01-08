
$(call standard_dirs)

$c_OBJECTS = frigg-arch-gdt.o \
	frigg-libc.o frigg-debug.o main.o multiboot.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

all-$c: $($c_BINDIR)/eir

$c_CXX = x86_64-elf-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include -I$(TREE_PATH)/eir/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -ffreestanding -m32 -fno-exceptions -fno-rtti -std=c++0x $(CXX_INCLUDE)
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_AS := x86_64-elf-as
$c_ASFLAGS := --32

$c_LD := x86_64-elf-ld
$c_LDFLAGS := -m elf_i386 -nostdlib -T $($c_SRCDIR)/link.ld

# note: we include libgcc manually here. impove that?
$c_LIBS := $(TREE_PATH)/$c/libgcc.a

$($c_BINDIR)/eir: $($c_OBJECT_PATHS) | $($c_BINDIR)
	$($d_LD) $($d_LDFLAGS) -o $@ $($d_OBJECT_PATHS) $($d_LIBS)

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_GENDIR)/frigg-arch-%.cpp: $(TREE_PATH)/frigg/src/arch_x86/%.cpp | $($c_GENDIR)
	install $< $@

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.asm
	$($d_AS) -o $@ $($d_ASFLAGS) $<

