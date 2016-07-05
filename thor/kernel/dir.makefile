
$(call standard_dirs)
$(call define_objdir,ARCH_OBJ,$($c_OBJDIR)/arch_x86)

$c_OBJECTS := frigg-debug.o frigg-libc.o \
	frigg-arch-gdt.o frigg-arch-idt.o frigg-arch-tss.o \
	arch_x86/early_stubs.o arch_x86/stubs.o \
	arch_x86/cpu.o arch_x86/trampoline.o \
	arch_x86/ints.o arch_x86/pic.o arch_x86/system.o arch_x86/paging.o \
	arch_x86/hpet.o \
	physical.o main.o service.o hel.o \
	core.o descriptor.o usermem.o schedule.o ring-buffer.o ipc.o \
	event.o thread.o rd.o io.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_HEADERS := thor.h
$c_HEADER_PATHS := $(addprefix $($c_HEADERDIR)/,$($c_HEADERS))

all-$c: $($c_BINDIR)/thor

install-$c-headers:
	install $($d_HEADER_PATHS) $(SYSROOT_PATH)/usr/include

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include -I$(TREE_PATH)/eir/include \
	-I$(TREE_PATH)/bragi/include -I$(TREE_PATH)/$c/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -O2
$c_CXXFLAGS += -fno-exceptions -fno-rtti
$c_CXXFLAGS += -ffreestanding -mno-red-zone -mcmodel=kernel
$c_CXXFLAGS += -msoft-float -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_AS := x86_64-managarm-as
$c_ASFLAGS :=

$c_LD := x86_64-managarm-ld
$c_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T $($c_SRCDIR)/link.ld

$($c_BINDIR)/thor: $($c_OBJECT_PATHS) $($c_SRCDIR)/link.ld | $($c_BINDIR)
	$($d_LD) $($d_LDFLAGS) -o $@ $($d_OBJECT_PATHS)

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_GENDIR)/frigg-arch-%.cpp: $(TREE_PATH)/frigg/src/arch_x86/%.cpp | $($c_GENDIR)
	install $< $@

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_OBJDIR) $($c_ARCH_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.asm | $($c_OBJDIR) $($c_OBJDIR)/arch_x86
	$($d_AS) -o $@ $($d_ASFLAGS) $<

