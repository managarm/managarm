
CXX = g++
CXX_INCLUDE =
CXX_FLAGS = -ffreestanding -m32 -fno-exceptions -fno-rtti -std=c++0x $(CXX_INCLUDE)

AS = as
AS_FLAGS = --32

LD_FLAGS = -m elf_i386 -nostdlib -T src/link.ld

KERNEL = bin/kernel.elf
OBJECTS = obj/frigg-gdt.o obj/main.o obj/multiboot.o

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src/arch_x86

.PHONY: all clean

all: $(KERNEL)

clean:
	rm -f $(KERNEL) $(OBJECTS)

$(KERNEL): $(OBJECTS)
	$(LD) $(LD_FLAGS) -o $(KERNEL) $(OBJECTS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@:obj/%.o=%.cpp)

obj/%.o: src/%.asm
	$(AS) $(AS_FLAGS) -o $@ $(SRCDIR)/$(@:obj/%.o=%.asm)

obj/frigg-%.o: $(FRIGG_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_SRCDIR)/$(patsubst obj/frigg-%.o,%.cpp,$@)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_SRCDIR)/$(@:obj/frigg-%.o=%.cpp)

-include $(OBJECTS:%.o=%.d)

# "special" dependencies: eir depends on thor and zisa
obj/multiboot.o: ../thor/bin/kernel.elf ../zisa/bin/init.elf

