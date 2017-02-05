
$(call standard_dirs)
$(call define_objdir,ARCH_OBJ,$($c_OBJDIR)/arch/x86)
$(call define_objdir,GENERIC_OBJ,$($c_OBJDIR)/generic)
$(call decl_targets,install-$c-headers)

$c_OBJECTS := frigg-debug.o frigg-libc.o \
	frigg-arch-gdt.o frigg-arch-idt.o frigg-arch-tss.o \
	arch/x86/early_stubs.o arch/x86/stubs.o \
	arch/x86/cpu.o arch/x86/trampoline.o \
	arch/x86/ints.o arch/x86/pic.o arch/x86/system.o arch/x86/paging.o \
	arch/x86/hpet.o \
	generic/physical.o generic/main.o generic/service.o generic/hel.o \
	generic/core.o generic/fiber.o generic/usermem.o generic/schedule.o \
	generic/futex.o generic/stream.o \
	generic/thread.o generic/irq.o generic/io.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

$c_HEADERS := thor.h
$c_HEADER_PATHS := $(addprefix $($c_HEADERDIR)/,$($c_HEADERS))

all-$c: $($c_BINDIR)/thor

install-$c-headers:
	install $($d_HEADER_PATHS) $(SYSROOT_PATH)/usr/include

$c_CXX = x86_64-managarm-kernel-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include -I$(TREE_PATH)/eir/include \
	-I$(TREE_PATH)/bragi/include -I$(TREE_PATH)/$c/include \
	-I$(TREE_PATH)/libarch/include -I$(CXXSHIM)/stage2/include -I$(FRIGG)/include \
	-I$($c_GENDIR) -iquote $($c_SRCDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -Wextra -O2
$c_CXXFLAGS += -fno-exceptions -fno-rtti
$c_CXXFLAGS += -ffreestanding -mno-red-zone -mcmodel=kernel
$c_CXXFLAGS += -msoft-float -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_AS := x86_64-managarm-kernel-as
$c_ASFLAGS :=

$c_LD := x86_64-managarm-kernel-ld
$c_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T $($c_SRCDIR)/arch/x86/link.x

$($c_BINDIR)/thor: $($c_OBJECT_PATHS) $($c_SRCDIR)/arch/x86/link.x | $($c_BINDIR)
	$($d_LD) $($d_LDFLAGS) -o $@ $($d_OBJECT_PATHS)

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_GENDIR)/frigg-arch-%.cpp: $(TREE_PATH)/frigg/src/arch_x86/%.cpp | $($c_GENDIR)
	install $< $@

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_GENERIC_OBJDIR) $($c_ARCH_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.asm | $($c_ARCH_OBJDIR)
	$($d_AS) -o $@ $($d_ASFLAGS) $<

# generate protobuf
gen-$c: $($c_GENDIR)/posix.frigg_pb.hpp $($c_GENDIR)/fs.frigg_pb.hpp \
	$($c_GENDIR)/mbus.frigg_pb.hpp

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<

